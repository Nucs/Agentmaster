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
                   be registered over the release family or vice versa), every icon file it names
                   is present (a layout without Images\ runs fine but has a BLANK taskbar and Start
                   icon), it isn't the folder the package already runs from, and the registered
                   package is a loose registration (an INSTALLED one is refused unless
                   -ReplaceInstalledPackage names its way back, see below);
    2. stamp     - write a version newer than the registered one into the layout's manifest
                   (development-mode re-registration refuses anything not newer; the build
                   itself always emits 0.0.1.0);
    3. stop      - end the running GUI process(es) from the OLD folder, and ONLY the GUI;
    4. drain     - wait for the old folder's OpenConsole.exe ConPTY hosts to exit on their own
                   (each is a process of the OLD package, so the register can't succeed before
                   they are gone);
    4b. remove   - ONLY with -ReplaceInstalledPackage, when the package is an INSTALLED one (see
                   below): Remove-AppxPackage the installed package, for this user only;
    5. register  - Add-AppxPackage -Register the new layout, which moves InstallLocation to it,
                   retried while Windows still reports the old package in use (0x80073D02);
    6. launch    - start the app by AUMID and wait for its new GUI process.

  The downtime is steps 3-6, typically a few seconds plus the app's own window restore. It grows
  with the number of tabs whose claude was running, because every host must end its tab's claude
  before it exits (about 1.5 minutes for 72 live hosts on a loaded machine, 2026-10-01).

  An INSTALLED package is different: the signed MSIX of a GitHub release, which Get-AppxPackage
  reports with IsDevelopmentMode False (SignatureKind Developer; a loose registration reads True /
  None). Windows registers no loose layout over an installed package of the same family:
  Add-AppxPackage -Register fails with 0x80073CFB, "The current user has already installed a
  packaged version of this app. An unpackaged version cannot replace this." (measured 2026-10-24:
  that swap stopped production, was refused at its register and relaunched the old version, and
  every turn it had cut off was lost for nothing). So step 1 refuses an installed package, before
  anything is stopped, unless -ReplaceInstalledPackage is given. With it, step 4b removes the
  installed package between the drain and the register, and -RollbackBundle names the bundle that
  brings it back.

  Rollback: the old folder is left untouched. When anything fails after the stop - a register
  that still fails after its retries, the removal, even the drain's process census (a WMI "Call
  cancelled" there on 2026-10-21 ended the script with production stopped and nothing relaunched) -
  the script brings the old version back up before it rethrows: it relaunches the still registered
  OLD version, or, when step 4b already removed the installed package, reinstalls -RollbackBundle
  and starts that. A failed swap costs a restart, never an outage (unless that reinstall fails too:
  the script then prints the commands to run by hand). To undo a successful swap later, run this
  script again with -LayoutDir pointing at the old folder; it gets the next version number, just
  like any other swap. An installed package's folder cannot be registered again (Windows removes
  it with the package), so after a -ReplaceInstalledPackage swap the undo is another loose layout,
  or a reinstall of the bundle once the loose registration is removed (Install-Agentmaster.ps1
  does that).

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
  How long to wait (default 120) for the old folder's OpenConsole.exe hosts to exit after the GUI
  stops. Each host ends its own clients and exits by itself. They are NEVER killed, because
  killing a host skips its CTRL_CLOSE broadcast and leaves that tab's pwsh + claude.exe running
  forever on a dead console (doc/agentmaster/OBSERVER.md section 13a).
  The drain has to finish before the register can: every host is a process of the OLD package,
  and Add-AppxPackage refuses to re-register a package while any of its processes runs
  (0x80073D02, "the following apps need to be closed"). A small workspace drains in seconds
  (11.7 s for 3 windows, 2026-09-26), but the time grows with the number of live tabs and the
  machine's load. On 2026-10-01, 72 hosts were still alive at 31.6 s and all were gone by about
  87 s; the old 30 s default sent the register into that refusal and the swap rolled back. When
  this timeout passes, the script still goes on to the register, whose retry window
  (-RegisterRetrySec) covers the stragglers.

.PARAMETER RegisterRetrySec
  How long (default 120) to keep retrying the register while Windows reports the OLD package
  still in use (0x80073D02: a ConPTY host, or another process of the old package, is still
  exiting). Counted from the first register attempt, retried every 3 seconds. Any other register
  failure, or this window running out, rolls back at once. 0 disables the retry.

.PARAMETER LaunchTimeoutSec
  How long to wait (default 60) for the new GUI process to appear after the AUMID launch. It
  only affects reporting: the app has been launched either way.

.PARAMETER NoLaunch
  Register the new layout but don't start the app. The downtime then lasts until someone starts
  it. A rollback then starts nothing either (a reinstall of -RollbackBundle still runs).

.PARAMETER ReplaceInstalledPackage
  Allow the swap when the registered package is an INSTALLED one (IsDevelopmentMode False: the
  signed MSIX of a GitHub release), by removing it in step 4b, after the drain and before the
  register. Without this switch such a package is refused in step 1, before anything is stopped,
  because Windows would refuse the register (0x80073CFB) only after the stop had cut off every
  running turn. Removing the package for this user needs no administrator rights and loses no
  state: Agentmaster keeps everything in its profile folder, and the package's own LocalState and
  RoamingState are empty (checked 2026-10-24). It does drop what Windows ties to the installed
  package: pins of the app to Start or the taskbar may have to be made again. Ignored, with a
  note, when the package is a development registration (the register updates that in place).

.PARAMETER RollbackBundle
  Required with -ReplaceInstalledPackage when the package is installed: the .msixbundle (or .msix)
  of the INSTALLED version, for example the release's Agentmaster_<version>.msixbundle that
  Install-Agentmaster.ps1 leaves in %TEMP%\Agentmaster-install. Once step 4b has removed the
  installed package, it is the only way back to that version, so step 1 checks it before anything
  is stopped: the file exists, its signature is valid and its signer trusted on this machine
  (Get-AuthenticodeSignature reads Valid; Add-AppxPackage refuses anything else), and its manifest
  names the same package at the installed version. Any failure after the removal reinstalls it
  (Add-AppxPackage -Path) and starts it.

.OUTPUTS
  PSCustomObject with Package, OldVersion, OldLocation, NewVersion, NewLocation, StoppedPids,
  DrainSeconds, HostsLeft (old OpenConsole hosts still alive when the drain ended),
  RegisterSeconds (including any in-use retries), RegisterAttempts, NewPid (0 when not launched
  or not seen within LaunchTimeoutSec), DowntimeSeconds (from the GUI stop until the new GUI
  process started; null when unknown), RemovedPackage (the installed package step 4b removed, ''
  when none) and RemoveSeconds. A failed swap throws instead of returning this object; its
  rollback is in the printed lines ("rollback: ...", or "ROLLBACK FAILED" with the commands).

.EXAMPLE
  # The whole hot deploy of the release package (hold the mutex around both lines):
  .\tools\Build-Agentmaster.ps1 -NoRestore -Configuration Release -ReleaseIdentity `
      -LayoutDir Q:\agentmaster-layouts\Release-B
  .\tools\Swap-AgentmasterLayout.ps1 -LayoutDir Q:\agentmaster-layouts\Release-B

.EXAMPLE
  # Dry run: resolve and validate everything, print what would happen, change nothing.
  .\tools\Swap-AgentmasterLayout.ps1 -LayoutDir Q:\agentmaster-layouts\Release-B -WhatIf

.EXAMPLE
  # The same hot deploy for the DEV identity (slots Q:\agentmaster-layouts\Debug and Debug-B): a Debug
  # build WITHOUT -ReleaseIdentity into the slot dev does not run from, then the swap with
  # -PackageName. Run it from a shell outside dev (the self-hosting guard refuses otherwise).
  # First run, 2026-10-05: 42 tabs, 16 live claudes, 8.2 s of downtime.
  .\tools\Build-Agentmaster.ps1 -NoRestore -LayoutDir Q:\agentmaster-layouts\Debug-B
  .\tools\Swap-AgentmasterLayout.ps1 -LayoutDir Q:\agentmaster-layouts\Debug-B -PackageName AgentmasterDev

.EXAMPLE
  # Production runs the INSTALLED MSIX of a GitHub release (IsDevelopmentMode False): move it to a
  # loose layout, with the bundle of the installed version as the way back.
  .\tools\Swap-AgentmasterLayout.ps1 -LayoutDir Q:\agentmaster-layouts\Release -ReplaceInstalledPackage `
      -RollbackBundle "$env:TEMP\Agentmaster-install\Agentmaster_0.8.0.0.msixbundle"

.NOTES
  Throws (exit code 1 when run as a script) when:
    - the layout folder or its AppxManifest.xml is missing, or the manifest can't be parsed;
    - the manifest's Identity Name differs from -PackageName;
    - an image the manifest names (Images\X.png) is missing from the layout, both as that file and
      as every resource variant (X.scale-200.png, ...): registering it would leave Agentmaster with
      a blank taskbar and Start icon;
    - -LayoutDir is the folder the package is already registered from (nothing to swap to);
    - the requested/derived version isn't newer than the registered one (or the revision would
      overflow 65535);
    - the calling process runs INSIDE the instance being stopped (see the self-hosting guard
      below). Run the swap from a shell outside it: the other identity's app, Windows Terminal,
      or a plain console;
    - the registered package is an INSTALLED one (IsDevelopmentMode False) and
      -ReplaceInstalledPackage is not given; or it is given and -RollbackBundle is missing, has no
      valid trusted signature, or names another package or version than the installed one;
    - Add-AppxPackage -Register fails with anything other than "package in use", or still reports
      the package in use when -RegisterRetrySec runs out. The old version is brought back first
      (see Rollback), then the error is rethrown;
    - anything else fails between the stop and the end of the register (the drain, step 4b's
      removal): the same rollback runs first, then the error is rethrown.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$LayoutDir,
    [ValidateSet('Agentmaster', 'AgentmasterDev')][string]$PackageName = 'Agentmaster',
    [version]$Version,
    [ValidateRange(0, 600)][int]$DrainTimeoutSec = 120,
    [ValidateRange(0, 600)][int]$RegisterRetrySec = 120,
    [ValidateRange(1, 600)][int]$LaunchTimeoutSec = 60,
    [switch]$NoLaunch,
    [switch]$ReplaceInstalledPackage,
    [string]$RollbackBundle
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

<#
.SYNOPSIS
  Tells whether a failed Add-AppxPackage means "the package is still in use". That is the one
  register failure worth waiting out, because it clears by itself as the old package's last
  processes (its draining ConPTY hosts) exit. Every other failure is final.
.PARAMETER ErrorRecord
  The ErrorRecord caught from Add-AppxPackage.
.OUTPUTS
  $true for HRESULT 0x80073D02 (ERROR_PACKAGES_IN_USE), $false for anything else. The message is
  checked as well as the HResult: the deployment error always prints the code in its text, while
  the HResult is only as reliable as the PowerShell hosting of the Appx module that raised it.
  (Under PowerShell 7, which this script requires, the literal 0x80073D02 is the Int32
  -2147009278, the same signed value Exception.HResult holds.)
#>
function Test-PackageInUseError([System.Management.Automation.ErrorRecord]$ErrorRecord) {
    return ($ErrorRecord.Exception.HResult -eq 0x80073D02) -or
        ($ErrorRecord.Exception.Message -match '0x80073D02')
}

<#
.SYNOPSIS
  Get-ProcessUnder for the process censuses that run while the old version is STOPPED: a failed
  query comes back as $null ("unknown") instead of a throw.
.DESCRIPTION
  On 2026-10-21 23:28 the drain's Get-CimInstance Win32_Process threw a WMI "Call cancelled" 17 s
  after the GUI stop. Under $ErrorActionPreference = 'Stop' that ended the script before its
  register, so nothing relaunched production and it stayed down for 12 minutes. A census only
  measures progress (the drain waits on it; the register retry and the launch wait report it), so a
  failed reading is simply taken again by the caller's loop. Each failure prints one line, which
  also tells the reader that the census, not the swap, had trouble.
.PARAMETER Folder
  Passed on to Get-ProcessUnder.
.PARAMETER Names
  Passed on to Get-ProcessUnder.
.PARAMETER What
  Which census this is, for the printed line ('drain', 'register retry', ...).
.OUTPUTS
  The matching Win32_Process instances as ONE array object (empty when none match; the leading
  comma keeps PowerShell from unrolling an empty array into $null), or $null when the query failed.
#>
function Get-ProcessUnderOrNull([string]$Folder, [string[]]$Names, [string]$What) {
    try {
        return , @(Get-ProcessUnder $Folder $Names)
    }
    catch {
        Write-Step "$($What): the process census failed ($($_.Exception.Message)) - taking it again"
        return $null
    }
}

<#
.SYNOPSIS
  Reads the package identity a .msixbundle or .msix file installs, without installing it.
.DESCRIPTION
  Both are zip files: a bundle carries AppxMetadata/AppxBundleManifest.xml (root <Bundle>), a single
  package AppxManifest.xml (root <Package>), and both name the package in <Identity>. PowerShell's
  XML adapter resolves elements by local name, so the manifests' default namespaces need no
  namespace manager.
.PARAMETER Path
  The absolute path of the .msixbundle / .msix.
.OUTPUTS
  [pscustomobject] with Name and Version (strings, as the manifest writes them).
.NOTES
  Throws when the file is not a readable zip, or holds neither manifest.
#>
function Read-PackageFileIdentity([string]$Path) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $zip.GetEntry('AppxMetadata/AppxBundleManifest.xml')
        if (-not $entry) { $entry = $zip.GetEntry('AppxManifest.xml') }
        if (-not $entry) { throw "'$Path' holds neither AppxMetadata/AppxBundleManifest.xml nor AppxManifest.xml - not an .msixbundle or .msix." }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { [xml]$xml = $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    finally { $zip.Dispose() }
    $id = if ($xml.Bundle) { $xml.Bundle.Identity } else { $xml.Package.Identity }
    return [pscustomobject]@{ Name = "$($id.Name)"; Version = "$($id.Version)" }
}

<#
.SYNOPSIS
  Keep the files an installed package holds in its own storage before step 4b removes it: Remove-AppxPackage deletes
  %LOCALAPPDATA%\Packages\<family>\ with the package (doc/agentmaster/PROFILES.md 2d; the install core's
  Save-AmPackageData is the same rule for the user-facing installers).
.DESCRIPTION
  Agentmaster's state lives in the profile, outside the package, so this storage is normally empty (measured 2026-10-26
  on this PC's production package: no file in LocalState, RoamingState or LocalCache). Should it hold any file (Windows
  Terminal's settings of a v0.1.x package, a file a process inside the package created under AppData), it is copied into
  <profile>\package-data\<family>-<yyyyMMdd-HHmmss>\<part>\ of the package's profile: its "<family>=" line in
  ~\.agentmaster.profiles, else %USERPROFILE%\.agentmaster (-dev for the AgentmasterDev family).
.PARAMETER Package
  The Get-AppxPackage object about to be removed.
.OUTPUTS
  [string] the folder the files were kept in, or '' when there was nothing to keep.
.NOTES
  Throws when the profile map cannot be read or a file cannot be copied - before the removal, so the swap's catch rolls
  back to the still-installed package.
#>
function Save-PackageOwnFiles($Package) {
    $family = [string]$Package.PackageFamilyName
    $root = Join-Path (Join-Path $env:LOCALAPPDATA 'Packages') $family
    $parts = @(@('LocalState', 'RoamingState', 'LocalCache') | Where-Object {
            $dir = Join-Path $root $_
            (Test-Path -LiteralPath $dir) -and @(Get-ChildItem -LiteralPath $dir -Recurse -Force -File -ErrorAction SilentlyContinue).Count -gt 0
        })
    if ($parts.Count -eq 0) { return '' }
    $profileDir = ''
    $map = Join-Path $env:USERPROFILE '.agentmaster.profiles'
    if (Test-Path -LiteralPath $map) {
        # ReadAllLines throws on a map it cannot read: never guess the profile the files go to.
        foreach ($line in [IO.File]::ReadAllLines($map)) {
            if ($line.StartsWith("$family=", [StringComparison]::OrdinalIgnoreCase)) { $profileDir = $line.Substring($family.Length + 1).Trim(); break }
        }
    }
    if (-not $profileDir) { $profileDir = Join-Path $env:USERPROFILE $(if ($family -like 'AgentmasterDev_*') { '.agentmaster-dev' } else { '.agentmaster' }) }
    $dest = Join-Path (Join-Path $profileDir 'package-data') ("$family-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $dest -ErrorAction Stop | Out-Null
    foreach ($part in $parts) {
        Copy-Item -LiteralPath (Join-Path $root $part) -Destination (Join-Path $dest $part) -Recurse -Force -ErrorAction Stop
    }
    return $dest
}

<#
.SYNOPSIS
  Brings the OLD version back up after a failure between the GUI stop and the end of the register,
  at most once per run.
.DESCRIPTION
  Two cases. While the old registration is still in place (every swap of a loose registration, and
  an installed package whose removal was never attempted or failed before taking effect), it relaunches
  that registration by AUMID: the behavior this script always had for a failed register. Once step 4b
  ATTEMPTED the removal of an installed package, the package may be gone, and then nothing can be
  relaunched, so it first reinstalls -RollbackBundle (Add-AppxPackage -Path) and starts that. A
  presence check that itself fails counts as "gone": reinstalling a version that is still installed
  changes nothing, while relaunching one that is gone would leave the app down.
  -NoLaunch suppresses the launch, never the reinstall. A failed reinstall or relaunch prints the
  command to run by hand instead of throwing, so the caller still sees the ORIGINAL error.
  Reads the script's state: $pkg, $oldDir, $oldVersion, $aumid, $bundlePath, $NoLaunch,
  $script:removeAttempted; sets $script:rolledBack.
#>
function Invoke-SwapRollback {
    if ($script:rolledBack) { return }
    $script:rolledBack = $true
    $restored = $false
    if ($script:removeAttempted) {
        $present = $false
        try { $present = [bool](Get-AppxPackage -Name $PackageName | Where-Object { $_.PackageFullName -eq $pkg.PackageFullName }) }
        catch { Write-Step "rollback: cannot tell whether $($pkg.PackageFullName) is still installed ($($_.Exception.Message)) - treating it as gone" }
        if (-not $present) {
            Write-Step "rollback: $($pkg.PackageFullName) is no longer installed - reinstalling it from $bundlePath"
            try {
                Add-AppxPackage -Path $bundlePath
                Write-Step "rollback: reinstalled $oldVersion"
                $restored = $true
            }
            catch {
                Write-Step "ROLLBACK FAILED: the reinstall failed ($($_.Exception.Message)). $PackageName is NOT INSTALLED now. By hand: Add-AppxPackage -Path '$bundlePath', then Start-Process '$aumid'"
                return
            }
        }
    }
    if (-not $oldDir -or $NoLaunch) {
        if ($NoLaunch) { Write-Step 'rollback: not relaunched (-NoLaunch)' }
        return
    }
    Write-Step ("rollback: relaunching the {0} {1}" -f $(if ($restored) { 'reinstalled' } else { 'still-registered' }), $oldVersion)
    try { Start-Process $aumid }
    catch { Write-Step "ROLLBACK: the relaunch failed ($($_.Exception.Message)) - start it by hand: Start-Process '$aumid'" }
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

# The app's ICONS. Every image the manifest names (Images\X.png: the Start, taskbar and tile logos
# and the StoreLogo) must exist in the layout, either as that exact file or as a resource variant
# (X.scale-200.png, X.targetsize-24_altform-unplated.png, ...), which is what resources.pri resolves
# the reference to. A layout without them still registers and runs, but Windows has no file to
# serve, so the taskbar and Start icons are BLANK. Production ran that way from slot B until
# 2026-10-02: CascadiaPackage.wapproj's image copy ignored the slot redirect, and the taskbar
# button showed nothing but the unread-count overlay. Checked here, before anything is stopped,
# so a bad layout costs nothing; -WhatIf reports it too.
$imageRefs = @([regex]::Matches((Get-Content -LiteralPath $newManifest -Raw), '(?i)Images\\[^"<>\s]+') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
$missingImages = @(foreach ($ref in $imageRefs) {
        $refDir = Join-Path $newDir (Split-Path $ref -Parent)
        $base = [IO.Path]::GetFileNameWithoutExtension($ref)
        $ext = [IO.Path]::GetExtension($ref)
        $exact = Test-Path -LiteralPath (Join-Path $newDir $ref) -PathType Leaf
        # "X.*.png" = X, then one or more qualifier segments, then the extension: every MakePri
        # variant name, and never a different logo that merely shares a prefix (X2.png).
        $variant = (Test-Path -LiteralPath $refDir -PathType Container) -and
            [bool](Get-ChildItem -LiteralPath $refDir -File -Filter "$base.*$ext" | Select-Object -First 1)
        if (-not ($exact -or $variant)) { $ref }
    })
if ($missingImages.Count -gt 0) {
    throw "Layout '$newDir' lacks the icon files its manifest names ($($missingImages -join ', ')), so Agentmaster would get a BLANK taskbar and Start icon. Rebuild it with the current CascadiaPackage.wapproj (its CopyAgentmasterImagesToLayout target fills Images\), or copy res\terminal\images-Dev\* into '$(Join-Path $newDir 'Images')'."
}

# The session database's import (SESSIONS_DB.md §4.2). A layout with agentmaster-sessiondb.exe but no duckdb.dll
# beside it registers and runs, but the session database can never start there (the GUI's copy-out refuses: "The
# package has no duckdb.dll"), so the AI search falls back to reading the transcripts and the cog's "On" does nothing.
# Every loose layout until 2026-10-17 was like that; CascadiaPackage.wapproj's CopyAgentmasterDuckDbToLayout target
# copies the DLL now. A WARNING, not a refusal: a ROLLBACK to an older slot (all of which lack the DLL) must stay
# possible, and a build from 2026-10-17 on falls back to reading the transcripts by itself
# (SearchDatabaseProfileIfUsable). Checked before anything is stopped, like the icons; -WhatIf reports it too.
$sessionDbExe = Join-Path $newDir 'agentmaster-sessiondb.exe'
$duckDbDll = Join-Path $newDir 'duckdb.dll'
if ((Test-Path -LiteralPath $sessionDbExe -PathType Leaf) -and -not (Test-Path -LiteralPath $duckDbDll -PathType Leaf)) {
    Write-Warning "Layout '$newDir' has agentmaster-sessiondb.exe but no duckdb.dll beside it, so the session database cannot start in this build (the AI search reads the transcripts instead). Rebuild it with the current CascadiaPackage.wapproj (its CopyAgentmasterDuckDbToLayout target copies the DLL), or copy dep\duckdb\<version>\<arch>\duckdb.dll into '$newDir'."
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

# An INSTALLED package (a GitHub release's signed MSIX, IsDevelopmentMode False) can't be replaced by
# a loose layout. Windows refuses the register with 0x80073CFB, and it does so only after the stop
# has cut off every running turn (2026-10-24 10:43). So it is refused HERE, before anything stops,
# unless the caller asked for step 4b's removal and named the bundle that can bring the installed
# version back. Under PowerShell 7 IsDevelopmentMode is a real Boolean (checked 2026-10-24); the
# explicit -eq $false keeps a missing property from ever reading as "installed".
$installedPackage = [bool]($pkg -and ($pkg.IsDevelopmentMode -eq $false))
$bundlePath = ''
$bundleSigner = ''
if ($installedPackage) {
    if (-not $ReplaceInstalledPackage) {
        throw "'$PackageName' is an INSTALLED package ($($pkg.PackageFullName), SignatureKind $($pkg.SignatureKind), IsDevelopmentMode False) - the signed MSIX of a release, not a loose registration. Windows registers no loose layout over it (0x80073CFB, 'An unpackaged version cannot replace this'), so this swap would stop the app and fail. Nothing was stopped. To move it to a loose layout, rerun with -ReplaceInstalledPackage -RollbackBundle <the .msixbundle of $($pkg.Version)>, which removes the installed package between the drain and the register."
    }
    if (-not $RollbackBundle) {
        throw "-ReplaceInstalledPackage needs -RollbackBundle: the .msixbundle of the installed $($pkg.Version) (Install-Agentmaster.ps1 leaves it in $env:TEMP\Agentmaster-install). Once the package is removed, only that bundle can bring this version back. Nothing was stopped."
    }
    $bundlePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RollbackBundle)
    if (-not (Test-Path -LiteralPath $bundlePath -PathType Leaf)) {
        throw "-RollbackBundle '$bundlePath' does not exist. Nothing was stopped."
    }
    # Valid = the signature checks out AND its signer is trusted on this machine, which is what
    # Add-AppxPackage requires of a signed package; a bundle that reads anything else could not be
    # reinstalled when it is needed.
    $signature = Get-AuthenticodeSignature -LiteralPath $bundlePath
    if ($signature.Status -ne 'Valid') {
        throw "-RollbackBundle '$bundlePath' has no valid trusted signature ($($signature.Status): $($signature.StatusMessage)), so Add-AppxPackage could not reinstall it. Nothing was stopped."
    }
    $bundleSigner = "$($signature.SignerCertificate.Subject)"
    $bundleIdentity = Read-PackageFileIdentity $bundlePath
    if ($bundleIdentity.Name -ne $PackageName -or [version]$bundleIdentity.Version -ne $oldVersion) {
        throw "-RollbackBundle '$bundlePath' holds $($bundleIdentity.Name) $($bundleIdentity.Version), not the installed $PackageName $oldVersion, so it could not put back what the removal takes away. Nothing was stopped."
    }
}
elseif ($ReplaceInstalledPackage) {
    Write-Step "note: -ReplaceInstalledPackage ignored - '$PackageName' is a development registration, which the register updates in place"
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
if ($installedPackage) {
    Write-Step "remove   $($pkg.PackageFullName) (an INSTALLED package) after the drain; the way back is $bundlePath ($oldVersion, signed by $bundleSigner)"
}
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
    DrainSeconds = 0.0; HostsLeft = 0; RegisterSeconds = 0.0; RegisterAttempts = 0; NewPid = 0
    DowntimeSeconds = $null; RemovedPackage = ''; RemoveSeconds = 0.0
}
# Invoke-SwapRollback's state: whether step 4b got as far as calling Remove-AppxPackage (from then on
# the installed package may be gone), and whether a rollback already ran (it runs at most once).
$script:removeAttempted = $false
$script:rolledBack = $false
if (-not $PSCmdlet.ShouldProcess("$PackageName GUI pid(s) $($guiIds -join ',')", 'stop, then re-register and relaunch')) {
    return $result
}
$stopAt = Get-Date
foreach ($g in $guis) {
    Stop-Process -Id $g.ProcessId -Force -ErrorAction SilentlyContinue
}
Write-Step "stopped  GUI pid(s) $($guiIds -join ', ')"

# Everything from here to the end of the register runs with the old version STOPPED, so any failure
# in it must bring the old version back up before it surfaces (Invoke-SwapRollback, in the catch at
# the end of step 5). On 2026-10-21 a throw in the drain's census skipped the register's own
# rollback, and production stayed down for 12 minutes with nobody told.
try {

# --- 4. drain ----------------------------------------------------------------------------------
# The hosts exit by themselves once their GUI's pipes close (each ends its own clients with
# CTRL_CLOSE). We only WAIT here and never kill them; see -DrainTimeoutSec for the reason. A census
# that fails (Get-ProcessUnderOrNull returns $null) leaves the last good count in place and is
# taken again a second later: it can only make the wait longer, never end it early.
$drain = [Diagnostics.Stopwatch]::StartNew()
$hosts = @()
if ($oldDir) {
    do {
        $census = Get-ProcessUnderOrNull $oldDir @('OpenConsole.exe') 'drain'
        if ($null -eq $census) { Start-Sleep -Seconds 1; continue }
        $hosts = $census
        if (-not $hosts) { break }
        Start-Sleep -Milliseconds 250
    } while ($drain.Elapsed.TotalSeconds -lt $DrainTimeoutSec)
}
$result.DrainSeconds = [math]::Round($drain.Elapsed.TotalSeconds, 1)
$result.HostsLeft = $hosts.Count
Write-Step ("drained  old ConPTY hosts in {0:n1}s ({1} still alive)" -f $drain.Elapsed.TotalSeconds, $hosts.Count)

# --- 4b. remove (an INSTALLED package, -ReplaceInstalledPackage) --------------------------------
# Step 1 let an installed package through only with -ReplaceInstalledPackage and a checked
# -RollbackBundle, so reaching this with $installedPackage means the caller asked for the removal.
if ($installedPackage) {
    # The removal ends whatever still runs of the old package, its ConPTY hosts included, and a host
    # ended that way skips its CTRL_CLOSE broadcast: its tab's pwsh + claude.exe would live on, on a
    # dead console (doc/agentmaster/OBSERVER.md section 13a). So hosts that outlived the drain get up
    # to -RegisterRetrySec more to exit by themselves first.
    if ($hosts.Count -gt 0) {
        $extra = [Diagnostics.Stopwatch]::StartNew()
        do {
            Start-Sleep -Milliseconds 500
            $census = Get-ProcessUnderOrNull $oldDir @('OpenConsole.exe') 'drain before the removal'
            if ($null -ne $census) { $hosts = $census }
        } while ($hosts.Count -gt 0 -and $extra.Elapsed.TotalSeconds -lt $RegisterRetrySec)
        $result.HostsLeft = $hosts.Count
        Write-Step ("drained  {0:n1}s more before the removal ({1} host(s) still alive)" -f $extra.Elapsed.TotalSeconds, $hosts.Count)
    }
    # The removal deletes the package's own storage with it: any file there is kept in the package's
    # profile first (Save-PackageOwnFiles). It throws BEFORE the removal, so a failure rolls back to the
    # still-installed package.
    $keptOwnFiles = Save-PackageOwnFiles $pkg
    if ($keptOwnFiles) { Write-Step "kept     the package's own files in $keptOwnFiles" }
    # From the first Remove-AppxPackage call on, the installed package may be gone, and the rollback
    # must then reinstall the bundle instead of relaunching (Invoke-SwapRollback reads this flag).
    $script:removeAttempted = $true
    $remove = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        try {
            Remove-AppxPackage -Package $pkg.PackageFullName
            break
        }
        catch {
            $removeError = $_
            # "In use" clears by itself as the old package's last processes exit, like the
            # register's refusal below; anything else is final.
            if ((Test-PackageInUseError $removeError) -and $remove.Elapsed.TotalSeconds -lt $RegisterRetrySec) {
                Write-Step 'remove: the old package is still in use - retrying'
                Start-Sleep -Seconds 3
                continue
            }
            Write-Step "REMOVE FAILED: $($removeError.Exception.Message)"
            throw
        }
    }
    # A bundle install also registers the bundle itself beside its main package (here
    # Agentmaster_0.8.0.0_neutral_~_56k4f06dsfp9r beside ..._x64__...). Removing the main package
    # normally takes the bundle with it (the usual way to uninstall a bundle-installed app), but an
    # entry left behind would make the register fail with 0x80073CFB all the same, so whatever is
    # still registered under the name is removed too.
    $leftovers = @(Get-AppxPackage -Name $PackageName -PackageTypeFilter Main, Bundle)
    foreach ($leftover in $leftovers) {
        Write-Step "remove: $($leftover.PackageFullName) is still registered - removing it too"
        Remove-AppxPackage -Package $leftover.PackageFullName
    }
    $remove.Stop()
    $result.RemovedPackage = $pkg.PackageFullName
    $result.RemoveSeconds = [math]::Round($remove.Elapsed.TotalSeconds, 1)
    Write-Step ("removed  {0} in {1:n1}s" -f $pkg.PackageFullName, $remove.Elapsed.TotalSeconds)
}

# --- 5. register -------------------------------------------------------------------------------
# Retried while Windows reports the OLD package still in use (0x80073D02). The drain above can end
# on its timeout with hosts still exiting, and while any process of the old package lives the
# deployment refuses. Waiting it out is far cheaper than rolling back: the rollback relaunches the
# old version, which restores its windows, and the next swap attempt then has to stop it (and drain
# whatever it started) all over again. That is what happened on 2026-10-01 under the old 30 s
# drain. Every other failure, or the retry window running out, rolls back at once.
$reg = [Diagnostics.Stopwatch]::StartNew()
while ($true) {
    $result.RegisterAttempts++
    try {
        Add-AppxPackage -Register $newManifest -ForceUpdateFromAnyVersion
        break
    }
    catch {
        # Captured first: the process census below runs pipelines of its own, and the rollback
        # message must describe THIS failure.
        $registerError = $_
        if ((Test-PackageInUseError $registerError) -and $reg.Elapsed.TotalSeconds -lt $RegisterRetrySec) {
            # The census only reports progress. The deployment's own answer decides, because a
            # process of the old package can also live outside the old folder. A failed census
            # ($null) reads "unknown" here and does not stop the retry.
            $left = if ($oldDir) { Get-ProcessUnderOrNull $oldDir @() 'register retry' } else { @() }
            Write-Step ("register attempt {0}: the old package is still in use ({1} process(es) left under the old folder) - retrying" -f $result.RegisterAttempts, $(if ($null -eq $left) { 'unknown' } else { $left.Count }))
            Start-Sleep -Seconds 3
            continue
        }
        # A failed update leaves the OLD registration in place (and after step 4b the bundle can
        # bring the removed one back), so bring the old version back up before surfacing the error.
        # A failed swap then costs one restart, not an outage.
        Write-Step "REGISTER FAILED: $($registerError.Exception.Message)"
        Invoke-SwapRollback
        throw
    }
}
$reg.Stop()

}
catch {
    # Any failure from the drain to the end of the register: the old version comes back first (a
    # no-op when the register's own failure path above already ran the rollback), then the ORIGINAL
    # error surfaces.
    Invoke-SwapRollback
    throw
}
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
    # The new version is registered and launched by now, so a failed census must not throw (the
    # caller would read a failed swap); it is taken again on the next pass.
    $found = Get-ProcessUnderOrNull $newDir @($apps | ForEach-Object { Split-Path $_.Executable -Leaf }) 'launch'
    if ($found) { $newGui = $found | Select-Object -First 1 }
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
