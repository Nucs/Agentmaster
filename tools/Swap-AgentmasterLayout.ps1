# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

#Requires -Version 7
<#
.SYNOPSIS
  Switch an installed Agentmaster package to a freshly built layout folder with only seconds of
  downtime: the swap half of the "hot build".

.DESCRIPTION
  An Agentmaster install on this machine is a LOOSE, development-mode registration of a package
  layout folder (Get-AppxPackage's InstallLocation). The running app locks that folder: its
  loaded images can't be overwritten and MakePri can't replace its memory-mapped resources.pri.
  Rebuilding INTO it therefore requires closing the app for the whole build, which is 15-30
  minutes when a header change forces a clean rebuild.

  The hot build avoids that. Build into a DIFFERENT layout folder while the app keeps running
  (tools\Build-Agentmaster.ps1 -LayoutDir <other folder>), then run this script to swap:

    1. validate  - the layout's AppxManifest.xml names the SAME package (a dev layout can never
                   be registered over the release family or vice versa), and it isn't the folder
                   the package already runs from;
    2. stamp     - write a version newer than the registered one into the layout's manifest
                   (development-mode re-registration refuses anything not newer; the build
                   itself always emits 0.0.1.0);
    3. stop      - end the running GUI process(es) from the OLD folder, and ONLY the GUI;
    4. drain     - wait for the old folder's OpenConsole.exe ConPTY hosts to exit on their own;
    5. register  - Add-AppxPackage -Register the new layout, which moves InstallLocation to it;
    6. launch    - start the app by AUMID and wait for its new GUI process.

  The downtime is steps 3-6, typically a few seconds plus the app's own window restore.

  Rollback: the old folder is left untouched. If step 5 fails, the script relaunches the still
  registered OLD version, so a failed swap costs a restart, never an outage. To undo a successful
  swap later, run this script again with -LayoutDir pointing at the old folder; it gets the next
  version number, just like any other swap.

  The caller must hold the build-launch mutex (tools\am-lock.sh) around build + swap. This script
  doesn't take it, so a caller can hold one lock across both halves.

.PARAMETER LayoutDir
  The freshly built package layout folder to switch to (it must contain AppxManifest.xml).
  Normally the folder you passed to Build-Agentmaster.ps1 -LayoutDir. A relative path is resolved
  against the current PowerShell location.

.PARAMETER PackageName
  Which installed identity to swap: 'Agentmaster' (release, the default) or 'AgentmasterDev'.
  Must match the Identity Name inside the layout's manifest; a mismatch is refused.

.PARAMETER Version
  The version to stamp and register. Default: the registered version with its revision (4th
  component) incremented, so 0.6.8.11 becomes 0.6.8.12. Keeping Major.Minor.Build unchanged
  matters because the in-app updater compares those three components against GitHub releases.
  An explicit value must be greater than the registered version.

.PARAMETER DrainTimeoutSec
  How long to wait (default 30) for the old folder's OpenConsole.exe hosts to exit after the GUI
  stops. Each host ends its own clients and exits by itself. They are NEVER killed, because
  killing a host skips its CTRL_CLOSE broadcast and leaves that tab's pwsh + claude.exe running
  forever on a dead console (doc/agentmaster/OBSERVER.md section 13a). When the timeout passes
  the swap goes ahead anyway: the new layout lives in a different folder, so a straggler locks
  nothing it needs, and the new instance's orphan reaper handles whatever the straggler leaves.

.PARAMETER LaunchTimeoutSec
  How long to wait (default 60) for the new GUI process to appear after the AUMID launch. It
  only affects reporting: the app has been launched either way.

.PARAMETER NoLaunch
  Register the new layout but don't start the app. The downtime then lasts until someone starts
  it.

.OUTPUTS
  PSCustomObject with Package, OldVersion, OldLocation, NewVersion, NewLocation, StoppedPids,
  DrainSeconds, HostsLeft (old OpenConsole hosts still alive when the drain ended),
  RegisterSeconds, NewPid (0 when not launched or not seen within LaunchTimeoutSec) and
  DowntimeSeconds (from the GUI stop until the new GUI process started; null when unknown).

.EXAMPLE
  # The whole hot deploy of the release package (hold the mutex around both lines):
  .\tools\Build-Agentmaster.ps1 -NoRestore -Configuration Release -ReleaseIdentity `
      -LayoutDir .\src\cascadia\CascadiaPackage\bin\x64\Release-B
  .\tools\Swap-AgentmasterLayout.ps1 -LayoutDir .\src\cascadia\CascadiaPackage\bin\x64\Release-B

.EXAMPLE
  # Dry run: resolve and validate everything, print what would happen, change nothing.
  .\tools\Swap-AgentmasterLayout.ps1 -LayoutDir .\src\cascadia\CascadiaPackage\bin\x64\Release-B -WhatIf

.NOTES
  Throws (exit code 1 when run as a script) when:
    - the layout folder or its AppxManifest.xml is missing, or the manifest can't be parsed;
    - the manifest's Identity Name differs from -PackageName;
    - -LayoutDir is the folder the package is already registered from (nothing to swap to);
    - the requested/derived version isn't newer than the registered one (or the revision would
      overflow 65535);
    - the calling process runs INSIDE the instance being stopped (see the self-hosting guard
      below). Run the swap from a shell outside it: the other identity's app, Windows Terminal,
      or a plain console;
    - Add-AppxPackage -Register fails. The old version is relaunched first, then the error is
      rethrown.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$LayoutDir,
    [ValidateSet('Agentmaster', 'AgentmasterDev')][string]$PackageName = 'Agentmaster',
    [version]$Version,
    [ValidateRange(0, 600)][int]$DrainTimeoutSec = 30,
    [ValidateRange(1, 600)][int]$LaunchTimeoutSec = 60,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'

<#
.SYNOPSIS
  Writes one timestamped progress line.
.PARAMETER Message
  The text to print after the [HH:mm:ss.fff] stamp.
#>
function Write-Step([string]$Message) {
    Write-Host ('[{0:HH:mm:ss.fff}] {1}' -f (Get-Date), $Message)
}

<#
.SYNOPSIS
  Lists processes whose image lives under a folder, optionally filtered by image name.
.PARAMETER Folder
  Absolute folder path. Matched as a prefix plus a trailing backslash, so ...\Release never
  matches ...\Release-B.
.PARAMETER Names
  Image names to keep (e.g. 'OpenConsole.exe'). Empty means any image.
.OUTPUTS
  Win32_Process CIM instances. Processes whose ExecutablePath can't be read (another user's,
  protected) are skipped. They can't be ours.
#>
function Get-ProcessUnder([string]$Folder, [string[]]$Names) {
    $prefix = $Folder.TrimEnd('\') + '\'
    Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and
        (-not $Names -or $Names -contains $_.Name)
    }
}

# --- 1. validate -------------------------------------------------------------------------------
# Resolved against the PowerShell location (not the process cwd, which can differ), and without
# requiring existence so the error below names the path the caller actually meant.
$newDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LayoutDir).TrimEnd('\')
$newManifest = Join-Path $newDir 'AppxManifest.xml'
if (-not (Test-Path -LiteralPath $newManifest -PathType Leaf)) {
    throw "No AppxManifest.xml in '$newDir' - build the layout first (Build-Agentmaster.ps1 -LayoutDir '$newDir')."
}
[xml]$manifestXml = Get-Content -LiteralPath $newManifest -Raw
$identity = $manifestXml.Package.Identity
if ($identity.Name -ne $PackageName) {
    # The guard that stops a DEV-identity layout (built without -ReleaseIdentity) from being
    # registered in place of the release app, or the reverse.
    throw "Layout '$newDir' is package '$($identity.Name)', not '$PackageName' - wrong identity (build it with$(if ($PackageName -eq 'Agentmaster') { '' } else { 'out' }) -ReleaseIdentity)."
}
$apps = @($manifestXml.Package.Applications.Application)
$appId = $apps[0].Id

$pkg = Get-AppxPackage -Name $PackageName | Select-Object -First 1
$oldDir = if ($pkg) { $pkg.InstallLocation.TrimEnd('\') } else { $null }
if ($oldDir -and [string]::Equals($oldDir, $newDir, [StringComparison]::OrdinalIgnoreCase)) {
    throw "'$PackageName' already runs from '$newDir'. A hot swap needs the OTHER folder; build into it with Build-Agentmaster.ps1 -LayoutDir."
}

# Development-mode re-registration only accepts a strictly newer version (the classic "increment
# the version number" refusal), and a brand-new version also sidesteps the same-full-name
# collision with an AppRepository staging folder still pinned by an orphaned claude.exe.
$oldVersion = if ($pkg) { [version]$pkg.Version } else { [version]'0.0.0.0' }
if (-not $Version) {
    if ($oldVersion.Revision -ge 65535) {
        throw "Registered version $oldVersion has no revision room left; pass -Version explicitly."
    }
    $Version = [version]::new($oldVersion.Major, $oldVersion.Minor, $oldVersion.Build, [math]::Max(0, $oldVersion.Revision) + 1)
}
elseif ($Version -le $oldVersion) {
    throw "-Version $Version must be newer than the registered $oldVersion."
}

# The GUI images to stop come from the OLD layout's own manifest. That keeps the stop exact across
# the WindowsTerminal.exe -> Agentmaster.exe rename; the fallback covers an unreadable manifest.
$guiNames = @('Agentmaster.exe', 'WindowsTerminal.exe')
if ($oldDir -and (Test-Path -LiteralPath (Join-Path $oldDir 'AppxManifest.xml'))) {
    try {
        [xml]$oldXml = Get-Content -LiteralPath (Join-Path $oldDir 'AppxManifest.xml') -Raw
        $named = @($oldXml.Package.Applications.Application | ForEach-Object { Split-Path $_.Executable -Leaf }) | Where-Object { $_ }
        if ($named) { $guiNames = @($named) }
    }
    catch {
        Write-Step "old manifest unreadable ($($_.Exception.Message)) - stopping by the default GUI names"
    }
}
# @(...) around the whole if: an `if` that yields nothing assigns $null, and piping $null still
# runs a ForEach-Object once, so without it the pid list below would hold a phantom $null entry.
$guis = @(if ($oldDir) { Get-ProcessUnder $oldDir $guiNames })

# Self-hosting guard. If THIS script descends from a GUI we are about to stop, the stop kills the
# script right after step 3 and leaves the package stopped and un-registered, i.e. a full outage.
# Walk our own ancestry and refuse.
# A script block on purpose. The property-name shorthand (ForEach-Object ProcessId) is a
# ShouldProcess operation: under -WhatIf it SKIPS the read, which left the pid list empty and
# quietly disabled this guard in exactly the dry run meant to exercise it.
$guiIds = @($guis | ForEach-Object { $_.ProcessId })
if ($guiIds) {
    $walk = Get-CimInstance Win32_Process -Filter "ProcessId=$PID"
    while ($walk) {
        if ($guiIds -contains $walk.ProcessId) {
            throw "This shell runs inside the $PackageName instance being swapped (pid $($walk.ProcessId)); stopping it would kill the swap midway. Run it from a shell outside that instance."
        }
        $walk = if ($walk.ParentProcessId) { Get-CimInstance Win32_Process -Filter "ProcessId=$($walk.ParentProcessId)" } else { $null }
    }
}

$pfn = (Get-AppxPackage -Name $PackageName | Select-Object -First 1).PackageFamilyName
if (-not $pfn) { $pfn = "$($PackageName)_56k4f06dsfp9r" }   # never registered: the Publisher-derived family
$aumid = "shell:appsFolder\$pfn!$appId"

Write-Step "package  $PackageName  $oldVersion @ $(if ($oldDir) { $oldDir } else { '(not registered)' })"
Write-Step "target   $Version @ $newDir"
Write-Step "running  $($guis.Count) GUI process(es) from the old folder: $(($guis | ForEach-Object { "$($_.Name)#$($_.ProcessId)" }) -join ', ')"

# --- 2. stamp ----------------------------------------------------------------------------------
if ($PSCmdlet.ShouldProcess($newManifest, "stamp Identity Version $Version")) {
    $identity.Version = $Version.ToString()
    $manifestXml.Save($newManifest)
}

# --- 3. stop (GUI only) ------------------------------------------------------------------------
$result = [pscustomobject]@{
    Package = $PackageName; OldVersion = $oldVersion; OldLocation = $oldDir
    NewVersion = $Version; NewLocation = $newDir; StoppedPids = @($guiIds)
    DrainSeconds = 0.0; HostsLeft = 0; RegisterSeconds = 0.0; NewPid = 0; DowntimeSeconds = $null
}
if (-not $PSCmdlet.ShouldProcess("$PackageName GUI pid(s) $($guiIds -join ',')", 'stop, then re-register and relaunch')) {
    return $result
}
$stopAt = Get-Date
foreach ($g in $guis) {
    Stop-Process -Id $g.ProcessId -Force -ErrorAction SilentlyContinue
}
Write-Step "stopped  GUI pid(s) $($guiIds -join ', ')"

# --- 4. drain ----------------------------------------------------------------------------------
# The hosts exit by themselves once their GUI's pipes close (each ends its own clients with
# CTRL_CLOSE). We only WAIT here and never kill them; see -DrainTimeoutSec for the reason.
$drain = [Diagnostics.Stopwatch]::StartNew()
$hosts = @()
if ($oldDir) {
    do {
        $hosts = @(Get-ProcessUnder $oldDir @('OpenConsole.exe'))
        if (-not $hosts) { break }
        Start-Sleep -Milliseconds 250
    } while ($drain.Elapsed.TotalSeconds -lt $DrainTimeoutSec)
}
$result.DrainSeconds = [math]::Round($drain.Elapsed.TotalSeconds, 1)
$result.HostsLeft = $hosts.Count
Write-Step ("drained  old ConPTY hosts in {0:n1}s ({1} still alive)" -f $drain.Elapsed.TotalSeconds, $hosts.Count)

# --- 5. register -------------------------------------------------------------------------------
$reg = [Diagnostics.Stopwatch]::StartNew()
try {
    Add-AppxPackage -Register $newManifest -ForceUpdateFromAnyVersion
}
catch {
    # The failed update left the OLD registration in place, so bring the old version back up
    # before surfacing the error. A failed swap then costs one restart, not an outage.
    Write-Step "REGISTER FAILED: $($_.Exception.Message)"
    if ($oldDir -and -not $NoLaunch) {
        Write-Step "rollback: relaunching the still-registered $oldVersion"
        Start-Process $aumid
    }
    throw
}
$reg.Stop()
$result.RegisterSeconds = [math]::Round($reg.Elapsed.TotalSeconds, 1)
$now = Get-AppxPackage -Name $PackageName | Select-Object -First 1
Write-Step ("registered {0} @ {1} in {2:n1}s" -f $now.Version, $now.InstallLocation, $reg.Elapsed.TotalSeconds)

# --- 6. launch ---------------------------------------------------------------------------------
if ($NoLaunch) {
    Write-Step 'not launched (-NoLaunch) - the app stays down until started'
    return $result
}
Start-Process $aumid
$wait = [Diagnostics.Stopwatch]::StartNew()
$newGui = $null
while (-not $newGui -and $wait.Elapsed.TotalSeconds -lt $LaunchTimeoutSec) {
    Start-Sleep -Milliseconds 200
    $newGui = Get-ProcessUnder $newDir @($apps | ForEach-Object { Split-Path $_.Executable -Leaf }) | Select-Object -First 1
}
if ($newGui) {
    $result.NewPid = $newGui.ProcessId
    # CreationDate is the kernel's own start stamp, so the downtime doesn't include this poll's lag.
    $result.DowntimeSeconds = [math]::Round(($newGui.CreationDate - $stopAt).TotalSeconds, 1)
    Write-Step ("launched pid {0} - downtime {1:n1}s (GUI stop -> new GUI start)" -f $newGui.ProcessId, $result.DowntimeSeconds)
}
else {
    Write-Step "launched via $aumid, but no new GUI appeared under the new folder within ${LaunchTimeoutSec}s - check it"
}
$result
