# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Agentmaster in-app installer payload (update / install / uninstall / start as the signed-in user).

.DESCRIPTION
    This script is NOT meant to be run by hand. It is embedded as a binary resource (AM_UPDATE_PS1,
    RT_RCDATA) inside the app exe (Agentmaster.exe), read at runtime by Updater.h / PortableInstall.h,
    materialized into the active profile dir or %TEMP%\Agentmaster-update, and run by the app.

    The block between the SHARED INSTALL CORE markers is VERBATIM in tools\Install-Agentmaster.ps1
    (tools\tests\Test-AgentmasterInstaller.ps1 checks); everything else is this script's own. Modes:

      (default)           the MSIX package update: download the bundle + certificate (SHA-256 verified),
                          trust the certificate (administrator approval once per release - every release
                          has its own; a user without an administrator at hand can SWITCH to the per-user
                          install instead when -ZipUrl is given: no administrator rights, the same data),
                          Add-AppxPackage (VCLibs / blocking-install recovery), relaunch.
      -Portable           the in-place update of a portable / per-user / all-users copy: download the zip,
                          replace ONLY what Agentmaster ships (the manifest, else the legacy list), keep
                          settings\, profile\, profile.path, install.path and a user's own files, stamp
                          .am-version, relaunch. A folder this account cannot write (the all-users install)
                          gets only its file swap elevated (one UAC prompt); the relaunch stays unelevated.
                          When that approval is declined or refused, or the swap fails, the user can ask
                          again, install the new version JUST FOR THEMSELVES instead (no administrator
                          rights, the same data; the all-users copy stays for the other users), or cancel.

    An update that does not end with the app started - it failed, was cancelled, or the user kept the
    installed version - starts the INSTALLED version again (Restart-PreviousApp): the app closed itself to
    let the update run, and must never be left closed. A failed swap first puts the previous version back
    (Install-AmLayout).
      -Uninstall          remove the MSIX package (per-user, no administrator rights).
      -UninstallPortable  remove a per-user install (-PortableDir): only its own shortcuts, verbs, Apps &
                          Features entry, PATH entry and shipped files.
      -UninstallMachine   the same for the all-users install (the app runs this ELEVATED).
      -InstallMachine     install the running unzip (-SourceDir) for all users into -DestDir (the app's
                          first-launch "Install for all users"; it runs this ELEVATED and waits).
      -RunAsMainUser      start -Exe (+ -ExeArgs, base64 of UTF-16LE) as the user signed in to Windows,
                          WITHOUT administrator rights (a one-shot scheduled task); hidden, never waits for
                          a key. Exit codes: 0 handed over, 2 cannot act for that user (not elevated),
                          3 nobody signed in, 4 the start failed.

    Profile data (e.g. %USERPROFILE%\.agentmaster) lives OUTSIDE every install and is never touched.

.PARAMETER BundleUrl
    The package update (default mode): the release's .msixbundle URL (a GitHub release-download URL; the
    app checks that before it passes one).
.PARAMETER CerUrl
    The package update: the release's Agentmaster.cer URL (its signing certificate).
.PARAMETER Version
    The release version text as tagged ("v0.8.0", "v0.8.0-prerelease-nightly"); shown, and its numeric
    part stamped into .am-version and Apps & Features.
.PARAMETER BundleSha256
    The package update: the bundle's digest ("sha256:<hex>" or bare hex); empty = no check.
.PARAMETER Family
    The package family name the package update / -Uninstall acts on (the release family by default).
.PARAMETER WaitPid
    The app process that started this script: waited for (up to 20 s) before files are replaced or removed.
.PARAMETER Uninstall
    Mode: remove the MSIX package.
.PARAMETER UninstallPortable
    Mode: remove the per-user install in -PortableDir.
.PARAMETER UninstallMachine
    Mode: remove the all-users install in -PortableDir (needs administrator rights).
.PARAMETER InstallMachine
    Mode: install -SourceDir for all users into -DestDir (needs administrator rights).
.PARAMETER RunAsMainUser
    Mode: start -Exe as the user signed in to Windows, without administrator rights.
.PARAMETER Portable
    Mode: update the copy in -PortableDir in place from -ZipUrl.
.PARAMETER PortableDir
    The folder of the copy that -Portable updates and -UninstallPortable / -UninstallMachine remove.
.PARAMETER SourceDir
    -InstallMachine: the layout to install (the unzip the app runs from).
.PARAMETER DestDir
    -InstallMachine: the destination; empty = the existing all-users install's folder, else
    Agentmaster in the 64-bit Program Files folder (%ProgramW6432%).
.PARAMETER Exe
    -RunAsMainUser: the program to start.
.PARAMETER ExeArgs
    -RunAsMainUser: the program's command-line tail, base64 of its UTF-16LE text (so no quoting survives
    two command lines); empty = none.
.PARAMETER ZipUrl
    -Portable: the release's portable zip for this architecture. In the package update it makes the
    "switch to the per-user install" answer available.
.PARAMETER ZipSha256
    The zip's digest ("sha256:<hex>" or bare hex); empty = no check.
.PARAMETER ProfileDir
    The profile folder the running app used (the app passes its own). A switch to the per-user install - from
    the package, or instead of an all-users copy that cannot be updated - points the new copy's profile.path at
    it, so every session and setting stays; empty = the package's choice, else the Default profile.

.NOTES
    Agentmaster -- a fork of Windows Terminal. Keep this file pure ASCII: the app writes it without a BOM,
    and Windows PowerShell 5.1 reads a BOM-less script in the ANSI code page.
#>
param(
    [string]$BundleUrl,
    [string]$CerUrl,
    [string]$Version      = '',
    [string]$BundleSha256 = '',
    [string]$Family       = 'Agentmaster_56k4f06dsfp9r',
    [int]   $WaitPid      = 0,
    [switch]$Uninstall,
    [switch]$UninstallPortable,
    [switch]$UninstallMachine,
    [switch]$InstallMachine,
    [switch]$RunAsMainUser,
    [switch]$Portable,
    [string]$PortableDir  = '',
    [string]$SourceDir    = '',
    [string]$DestDir      = '',
    [string]$Exe          = '',
    [string]$ExeArgs      = '',
    [string]$ZipUrl       = '',
    [string]$ZipSha256    = '',
    [string]$ProfileDir   = ''
)

$ErrorActionPreference = 'Stop'
$Aumid = "$Family!App"
$Dir   = Join-Path $env:TEMP 'Agentmaster-update'
# The two update modes (the package update and -Portable): the app closed itself to let them run, so they
# end with an app started (Restart-PreviousApp). Every other mode was asked for something else.
$script:AmIsUpdateRun = -not ($Uninstall -or $UninstallPortable -or $UninstallMachine -or $InstallMachine -or $RunAsMainUser)
# Set once this run started an Agentmaster (the updated one, a per-user copy, or the previous version again).
$script:AmAppStarted = $false

# >>>>>>>> AGENTMASTER SHARED INSTALL CORE >>>>>>>>
# This block is VERBATIM in tools\Install-Agentmaster.ps1 and in
# src\cascadia\TerminalApp\AgentMaster\am-update.ps1 (the installer payload compiled into Agentmaster.exe).
# Edit it in one, copy it to the other: tools\tests\Test-AgentmasterInstaller.ps1 fails when the two copies
# differ by one byte. Both files must stay pure ASCII: Windows PowerShell 5.1 reads a BOM-less script in the
# ANSI code page, and the app writes am-update.ps1 without a BOM.
#
# What the core knows (doc/agentmaster/PROFILES.md section 2b/2c):
#   * THREE install kinds a script can make: a PER-USER copy (no administrator rights - the preferred one), an
#     ALL-USERS copy (Program Files + machine-wide registration; administrator rights) and the MSIX package
#     (the script-specific code; its certificate needs administrator rights once per release).
#   * A per-user copy has the exact shape the app's own first-launch installer (PortableInstall.h) makes:
#     the binaries, install.path = installed, profile.path -> the profile the MSIX package used (else the
#     Default profile), Start-menu + desktop shortcuts, the "Open in Agentmaster" verbs, an Apps & Features
#     entry whose UninstallString is "<exe>" --uninstall-portable, and the folder on the user PATH. An
#     all-users copy is the same one level up, with install.path = machine and no profile.path, and its
#     shortcuts and Apps & Features entry are named "Agentmaster (all users)" (the two can coexist).
#   * Updates and uninstalls touch ONLY what Agentmaster ships (the .am-manifest list, else a fixed list of
#     the names releases have shipped), so a user's own files in the same folder survive. An update moves
#     the previous version aside first and puts it back when the new one cannot be laid in.
#   * An install is made for the user SIGNED IN to Windows, not for whatever account runs the script: an
#     elevated or other-account run hands the per-user part to that user (a one-shot scheduled task), and
#     the app is always started without administrator rights.

# ---- tiny console UI ------------------------------------------------------------------------------------
<#
.SYNOPSIS
    A console line that heads a phase of the work ("==> ..."). The Write-Ok / Write-Warn / Write-Info
    lines below are its details. All four are captured by Start-Transcript, which is how a hidden phase
    reports back to the script that waits for it.
.PARAMETER m
    The message.
.NOTES
    Never throws (Write-Host only).
#>
function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
<#
.SYNOPSIS
    A detail line for something that worked (green).
.PARAMETER m
    The message.
.NOTES
    Never throws.
#>
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
<#
.SYNOPSIS
    A detail line for something the user should notice: a step that did not fully work, or a choice made
    for them (yellow).
.PARAMETER m
    The message.
.NOTES
    Never throws.
#>
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }
<#
.SYNOPSIS
    A quiet detail line: background facts and hints (dark gray).
.PARAMETER m
    The message.
.NOTES
    Never throws.
#>
function Write-Info { param($m) Write-Host "    $m" -ForegroundColor DarkGray }

# ---- environment helpers --------------------------------------------------------------------------------
<#
.SYNOPSIS
    An absolute file-system path for $Path, resolved against PowerShell's CURRENT LOCATION.
.DESCRIPTION
    .NET file APIs ([IO.File], GetFullPath) resolve a relative path against the PROCESS directory, which
    PowerShell does not keep in step with its location - so every path handed to .NET goes through here.
.PARAMETER Path
    A relative or absolute path (it need not exist).
.OUTPUTS
    [string]
#>
function Resolve-AmPath {
    param([string]$Path)
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

<#
.SYNOPSIS
    Enable TLS 1.2 for downloads (GitHub refuses older protocols; Windows PowerShell 5.1 may default to them).
.NOTES
    Never throws: a failure leaves the protocol set as it was.
#>
function Initialize-Net {
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch { Write-Info "TLS 1.2 could not be enabled: $($_.Exception.Message)" }
}

<#
.SYNOPSIS
    Make the Appx cmdlets available (PowerShell 7 reaches them through the Windows PowerShell compatibility
    session; Windows PowerShell has them natively).
.NOTES
    Never throws: a failure surfaces later as "Add-AppxPackage is not recognized".
#>
function Import-AppxModule {
    if ($PSVersionTable.PSVersion.Major -ge 7) {
        Import-Module Appx -UseWindowsPowerShell -WarningAction SilentlyContinue -ErrorAction SilentlyContinue
    }
}

<#
.SYNOPSIS
    The OS architecture as a release asset suffix: arm64, x64 or x86.
.OUTPUTS
    [string]
#>
function Get-OSArch {
    switch ($env:PROCESSOR_ARCHITECTURE) {
        'ARM64' { 'arm64' }
        'AMD64' { 'x64' }
        'x86'   { 'x86' }
        default { 'x64' }
    }
}

<#
.SYNOPSIS
    True when THIS process runs with administrator rights (an elevated token).
.OUTPUTS
    [bool]
#>
function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

<#
.SYNOPSIS
    True when the account running this process COULD elevate by itself: it is a member of Administrators,
    whose rights UAC filtered out ("Group used for deny only"), so a UAC prompt is a single consent click.
.DESCRIPTION
    False for a standard user, whose UAC prompt asks for ANOTHER account's credentials - the case where the
    scripts offer the no-administrator per-user install first.
.OUTPUTS
    [bool]
#>
function Test-CanElevate {
    if (Test-Admin) { return $true }
    try {
        # /nh + our own header: the CSV column titles are translated on a non-English Windows.
        $groups = & "$env:SystemRoot\System32\whoami.exe" /groups /fo csv /nh 2>$null | ConvertFrom-Csv -Header 'Name', 'Type', 'Sid', 'Attributes'
        return [bool]($groups | Where-Object { $_.Sid -eq 'S-1-5-32-544' })
    } catch {
        return $false
    }
}

<#
.SYNOPSIS
    Who runs this script and who is signed in to its Windows session (the "main user").
.DESCRIPTION
    An install sets Agentmaster up for the account that RUNS it (HKCU, its Start menu, its profile), so a
    script started with an administrator's UAC credentials, with "Run as different user", or by a deployment
    agent as SYSTEM must not install for that account. The main user is the owner of explorer.exe in this
    session; in a service session (0) or without an explorer there, the user at the console
    (Win32_ComputerSystem.UserName). Every probe is best-effort; an unknown main user reads as "this account"
    (IsMainUser = true), so a lookup failure never redirects an install.
.OUTPUTS
    [pscustomobject] RunningSid, RunningName, Elevated, IsSystem, SessionId, MainUserSid, MainUserName,
    MainUserProfile (the main user's profile folder), IsMainUser.
#>
function Get-AmIdentity {
    $me = [Security.Principal.WindowsIdentity]::GetCurrent()
    $elevated = (New-Object Security.Principal.WindowsPrincipal $me).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $session = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
    $main = $null
    try {
        if ($session -ne 0) {
            $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe' AND SessionId=$session" -ErrorAction Stop | Select-Object -First 1
            if ($explorer) {
                $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwnerSid -ErrorAction Stop
                if ($owner.Sid) { $main = [string]$owner.Sid }
            }
        }
        if (-not $main) {
            $console = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).UserName
            if ($console) {
                $main = (New-Object Security.Principal.NTAccount($console)).Translate([Security.Principal.SecurityIdentifier]).Value
            }
        }
    } catch {
        Write-Info "the signed-in user could not be determined ($($_.Exception.Message)); installing for this account"
    }
    $mainName = $null
    $mainProfile = $null
    if ($main) {
        try { $mainName = (New-Object Security.Principal.SecurityIdentifier($main)).Translate([Security.Principal.NTAccount]).Value } catch { $mainName = $main }
        try { $mainProfile = (Get-CimInstance Win32_UserProfile -Filter "SID='$main'" -ErrorAction Stop | Select-Object -First 1).LocalPath } catch { $mainProfile = $null }
    }
    [pscustomobject]@{
        RunningSid      = $me.User.Value
        RunningName     = $me.Name
        Elevated        = [bool]$elevated
        IsSystem        = [bool]$me.IsSystem
        SessionId       = $session
        MainUserSid     = $main
        MainUserName    = $mainName
        MainUserProfile = $mainProfile
        IsMainUser      = (-not $main) -or ($main -eq $me.User.Value)
    }
}

# ---- downloading ---------------------------------------------------------------------------------------
<#
.SYNOPSIS
    The SHA-256 of a file as upper-case hex.
.DESCRIPTION
    Raw .NET, NOT Get-FileHash: that cmdlet lives in a MODULE whose auto-load can fail in Windows PowerShell
    5.1 under a PS7-polluted PSModulePath (observed live: "The term 'Get-FileHash' is not recognized" killed
    a whole install). .NET needs no module in any host.
.PARAMETER Path
    The file to hash.
.OUTPUTS
    [string]
.NOTES
    Throws when the file cannot be opened.
#>
function Get-FileSha256 {
    param($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs  = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Path).Path)
    try     { return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '') }
    finally { $fs.Dispose(); $sha.Dispose() }
}

# ---- certificate trust (the only step of a PACKAGE install that needs administrator rights) ------------
# AppX deployment validates the package signature in SYSTEM context, so the cert must be trusted
# MACHINE-wide (LocalMachine TrustedPeople or Root) - a CurrentUser store never satisfies it. Every release
# is signed with its OWN certificate (release.yml mints one per run unless a signing PFX is configured), so
# this happens once PER RELEASE, not once ever. All store access is raw .NET X509Store, NOT the Cert:\ drive:
# the drive needs Microsoft.PowerShell.Security, whose load can FAIL in Windows PowerShell 5.1 when a
# PS7-polluted PSModulePath shadows it.
<#
.SYNOPSIS
    True when the LocalMachine store $StoreName holds a certificate with $Thumbprint.
.PARAMETER Thumbprint
    The certificate's SHA-1 thumbprint (hex).
.PARAMETER StoreName
    The LocalMachine store: TrustedPeople or Root.
.OUTPUTS
    [bool]; false on any store error (it never throws).
#>
function Test-CertInMachineStore {
    param($Thumbprint, $StoreName)
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($StoreName, 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
        return ($store.Certificates.Find('FindByThumbprint', $Thumbprint, $false).Count -gt 0)
    } catch {
        return $false
    } finally {
        $store.Close()
    }
}

<#
.SYNOPSIS
    True when the certificate file is already trusted machine-wide (TrustedPeople or Root).
.PARAMETER CerPath
    The .cer file.
.OUTPUTS
    [bool]
.NOTES
    Throws when the file cannot be read as a certificate.
#>
function Test-CertTrustedMachine {
    param($CerPath)
    $full = (Resolve-Path -LiteralPath $CerPath).Path
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $full
    foreach ($store in 'TrustedPeople', 'Root') {
        if (Test-CertInMachineStore $cert.Thumbprint $store) { return $true }
    }
    return $false
}

<#
.SYNOPSIS
    Trust the signing certificate machine-wide (LocalMachine\TrustedPeople), elevating ONLY for that import.
.DESCRIPTION
    The elevated child imports only the certificate; the package itself is then added by THIS process, so it
    lands on the invoking user even when UAC was answered with another account's credentials.
.PARAMETER CerPath
    The .cer file.
.NOTES
    Throws when the elevation is declined, the import fails, or the import did not take effect.
#>
function Approve-SigningCert {
    param($CerPath)
    $full = (Resolve-Path -LiteralPath $CerPath).Path
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $full
    $thumb = $cert.Thumbprint
    if (Test-CertTrustedMachine $full) {
        Write-Ok "signing cert already trusted ($thumb)"
        return
    }
    Write-Step "Trusting this release's signing certificate (needs administrator approval)"
    if (Test-Admin) {
        $st = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPeople', 'LocalMachine')
        try { $st.Open('ReadWrite'); $st.Add($cert) } finally { $st.Close() }
    } else {
        $p = $full.Replace("'", "''")
        $inner = "try { " +
                 "`$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 '$p'; " +
                 "`$s = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPeople', 'LocalMachine'); " +
                 "`$s.Open('ReadWrite'); `$s.Add(`$c); `$s.Close(); exit 0 } catch { exit 7 }"
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))
        $exit = 0
        try {
            $exit = Invoke-AmElevatedPowerShell -Arguments "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $enc"
        } catch {
            # A refusal by policy (1260) is not a cancel: say so, the per-user install is the way out there.
            if ((Get-AmWin32ErrorCode -ErrorRecord $_ -Default 1223) -eq 1260) {
                throw "Windows does not let this account run programs as administrator (a setting of this PC), and the MSIX package needs each release's signing certificate trusted for the whole PC. Install just for you instead: it needs no administrator rights."
            }
            throw "Administrator elevation was cancelled; the signing certificate must be trusted (machine-wide) to install the MSIX."
        }
        if ($exit -ne 0) { throw "Failed to import the signing certificate (elevated import returned $exit)." }
    }
    if (-not (Test-CertInMachineStore $thumb 'TrustedPeople')) {
        throw "Certificate import did not take effect."
    }
    Write-Ok "trusted ($thumb)"
}

<#
.SYNOPSIS
    Download (once) the Microsoft.VCLibs framework package for this OS architecture into $Dir.
.PARAMETER Dir
    The download cache folder.
.OUTPUTS
    [string] the .appx path.
.NOTES
    Throws when the download fails.
#>
function Get-VCLibs {
    param($Dir)
    $arch = Get-OSArch
    $dest = Join-Path $Dir "Microsoft.VCLibs.$arch.14.00.Desktop.appx"
    if (-not (Test-Path $dest)) {
        Write-Info "fetching dependency Microsoft.VCLibs ($arch)"
        $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        try {
            Invoke-WebRequest -Uri "https://aka.ms/Microsoft.VCLibs.$arch.14.00.Desktop.appx" -OutFile $dest -UseBasicParsing
        } finally { $ProgressPreference = $old }
    }
    return $dest
}

# ---- the MSIX package ------------------------------------------------------------------------------------
<#
.SYNOPSIS
    The registered package of a family for THIS user, or $null.
.PARAMETER FamilyName
    A package family name, e.g. Agentmaster_56k4f06dsfp9r.
.OUTPUTS
    The Get-AppxPackage object, or $null.
#>
function Get-AmPackage {
    param([string]$FamilyName)
    Get-AppxPackage -ErrorAction SilentlyContinue | Where-Object { $_.PackageFamilyName -eq $FamilyName } | Select-Object -First 1
}

<#
.SYNOPSIS
    Remove a registered package for THIS user (per-user removal, no administrator rights), after closing
    the instances that run from it. Its data (the profile folder) lives outside the package and is kept.
.PARAMETER Package
    The Get-AppxPackage object.
.NOTES
    Throws when Remove-AppxPackage fails.
#>
function Remove-AmPackage {
    param($Package)
    if (-not $Package) { return }
    if ($Package.InstallLocation) { Stop-ProcessesUnder -DirPath $Package.InstallLocation }
    Remove-AppxPackage -Package $Package.PackageFullName -ErrorAction Stop
    Write-Ok ("removed the package " + $Package.PackageFullName)
}

# ---- processes -----------------------------------------------------------------------------------------
<#
.SYNOPSIS
    Close the Agentmaster GUI processes that run from $DirPath and wait for their ConPTY hosts to drain.
.DESCRIPTION
    Kills ONLY the GUI process (Agentmaster.exe / a pre-rename WindowsTerminal.exe), path-filtered, so the
    Store Windows Terminal and every other install are never touched - and NEVER its OpenConsole.exe hosts:
    a host killed outright skips its CTRL_CLOSE broadcast and leaves its tab's pwsh + claude.exe running
    forever with a DEAD console (2026-09-04: 35 pairs + ~140 children, alive six days). Killing the GUI
    closes its pipe handles; each host then ends its clients itself and exits. A host still alive after 30 s
    is wedged and is killed as the last resort.
    An all-users copy can run for OTHER users of the PC at the same time; closing their windows would lose
    their work, so that case throws unless -AllowOtherUsers.
.PARAMETER DirPath
    The install folder.
.PARAMETER AllowOtherUsers
    Close other users' instances too (an explicit -Force of the caller).
.NOTES
    Throws only for other users' instances without -AllowOtherUsers.
#>
function Stop-ProcessesUnder {
    param($DirPath, [switch]$AllowOtherUsers)
    if (-not $DirPath) { return }
    $prefix = (Resolve-AmPath $DirPath).TrimEnd('\') + '\'
    $gui = @(Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe' OR Name='WindowsTerminal.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
    if (-not $AllowOtherUsers) {
        $mine = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        $others = @($gui | Where-Object {
            $sid = $null
            try { $sid = (Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid -ErrorAction Stop).Sid } catch { $sid = $null }
            $sid -and $sid -ne $mine
        })
        if ($others.Count -gt 0) {
            # Worded for both scripts: the in-app updater has no -Force, Install-Agentmaster.ps1 has one.
            throw "Agentmaster is running for $($others.Count) other user session(s) from $DirPath. They must close it first (Install-Agentmaster.ps1 -Force closes their windows instead)."
        }
    }
    foreach ($p in $gui) {
        Write-Warn "closing running instance (pid $($p.ProcessId))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    $deadline = (Get-Date).AddSeconds(30)
    do {
        $hosts = @(Get-CimInstance Win32_Process -Filter "Name='OpenConsole.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
        if ($hosts.Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    foreach ($p in $hosts) {
        Write-Warn "ConPTY host pid $($p.ProcessId) did not exit in 30 s; killing it (its tab's session processes may be orphaned)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($gui.Count -gt 0 -or $hosts.Count -gt 0) { Start-Sleep -Milliseconds 700 }
}

# ---- what Agentmaster ships ------------------------------------------------------------------------------
# The exe-side USER STATE an upgrade or uninstall never removes (PortableInstall.h IsPreservedLeaf):
# settings\ (a pre-choice portable's Terminal settings), profile\ (a self-contained profile = the user's
# sessions), profile.path (which profile this copy uses), install.path (the install decision).
$script:AmPreservedNames = @('settings', 'profile', 'profile.path', 'install.path')

# The top-level names releases have shipped, for an install made from a zip WITHOUT a manifest (zips built
# before 2026-10-21). Read from the v0.7.10 x64 zip, plus the names newer layouts add. Exact names, plus one
# pattern: never a broad *.dll / *.exe, which would also match a user's own files in a shared folder.
$script:AmLegacyShippedNames = @(
    '.am-manifest', '.am-version', '.portable', 'Agentmaster.exe', 'NOTICE.html', 'OpenConsole.exe',
    'OpenConsoleProxy.dll', 'TerminalApp.dll', 'TerminalConnection.dll', 'TerminalThemeHelpers.dll',
    'WindowsTerminal.exe', 'WindowsTerminalShellExt.dll', 'agentmaster-cli.exe', 'agentmaster-sessiondb.exe',
    'duckdb.dll', 'defaults.json', 'elevate-shim.exe', 'resources.pri', 'wtd.exe', 'wt.exe', 'Images',
    'Microsoft.UI.Xaml', 'Microsoft.UI.Xaml.dll', 'ProfileGeneratorIcons', 'ProfileIcons')
$script:AmLegacyShippedPatterns = @('Microsoft.Terminal.*.dll')

# The folder an upgrade moves the previous version into while it lays the new one in (Install-AmLayout):
# this prefix + 8 hex digits, inside the install folder - the same volume, so every move is a rename. It is
# Agentmaster's own (an uninstall removes it; the next upgrade deletes one an earlier run could not).
# PortableInstall.h kSetAsideLeafPrefix is the same name.
$script:AmSetAsidePrefix = '.am-previous-'

<#
.SYNOPSIS
    True when $Name is a plain top-level entry name a manifest may list (no separator, drive or stream
    colon, wildcard or control character; not . or ..; no trailing dot or space; not a DOS device name).
.DESCRIPTION
    An updater or uninstaller deletes exactly the names a manifest lists, so a corrupt or planted manifest
    must never be able to name anything but a plain entry of its own folder - not a preserved one through
    Windows' trailing-dot/space stripping, not a device (PortableInstall.h IsSafeManifestName, the same rule).
.PARAMETER Name
    One trimmed manifest line.
.OUTPUTS
    [bool]
#>
function Test-SafeManifestName {
    param([string]$Name)
    if (-not $Name -or $Name -eq '.' -or $Name -eq '..') { return $false }
    if ($Name -match '[\\/:*?"<>|]') { return $false }
    foreach ($c in $Name.ToCharArray()) { if ([int]$c -lt 32) { return $false } }
    # Windows strips a trailing dot or space from a path's last segment: "profile." would name the
    # PRESERVED profile folder (Remove-Item happens to refuse it today - the rule must not rest on that).
    if ($Name.EndsWith('.') -or $Name.EndsWith(' ')) { return $false }
    # A DOS device name, with or without an extension, names a device, not a file.
    if ((($Name -split '\.')[0]) -match '^(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])$') { return $false }
    return $true
}

<#
.SYNOPSIS
    The names <Dir>\.am-manifest lists, validated; an empty array when there is no manifest.
.PARAMETER Dir
    An install or layout folder.
.OUTPUTS
    [string[]]
#>
function Read-AmManifest {
    param([string]$Dir)
    $path = Resolve-AmPath (Join-Path $Dir '.am-manifest')
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $t = $t.TrimEnd('/', '\')
        if (-not (Test-SafeManifestName $t)) { continue }
        if (-not ($names | Where-Object { $_ -ieq $t })) { $names.Add($t) }
    }
    return $names.ToArray()
}

<#
.SYNOPSIS
    The top-level names in $Dir that Agentmaster shipped: the manifest's list when there is one, else the
    entries of $Dir that match the legacy list. Never a preserved name.
.PARAMETER Dir
    An install folder.
.OUTPUTS
    [string[]] names that exist in $Dir (or that the manifest lists).
#>
function Get-AmShippedNames {
    param([string]$Dir)
    $listed = @(Read-AmManifest -Dir $Dir)
    if ($listed.Count -gt 0) {
        return @($listed | Where-Object { $script:AmPreservedNames -notcontains $_ })
    }
    $found = @()
    foreach ($entry in @(Get-ChildItem -LiteralPath $Dir -Force -ErrorAction SilentlyContinue)) {
        $n = $entry.Name
        if ($script:AmPreservedNames -contains $n) { continue }
        $ours = ($script:AmLegacyShippedNames | Where-Object { $_ -ieq $n }) -or
                ($script:AmLegacyShippedPatterns | Where-Object { $n -like $_ })
        if ($ours) { $found += $n }
    }
    return $found
}

<#
.SYNOPSIS
    Remove the given top-level names from $Dir - never a preserved name, never anything not listed.
.PARAMETER Dir
    The folder.
.PARAMETER Names
    The names to remove (each must pass Test-SafeManifestName).
.OUTPUTS
    [string[]] the names that were removed.
.NOTES
    Never throws for a single entry (a locked file is reported and skipped); the caller checks the result.
#>
function Remove-AmEntries {
    param([string]$Dir, [string[]]$Names)
    $removed = @()
    foreach ($n in $Names) {
        if (-not (Test-SafeManifestName $n)) { continue }
        if ($script:AmPreservedNames -contains $n) { continue }
        $p = Join-Path $Dir $n
        if (-not (Test-Path -LiteralPath $p)) { continue }
        try {
            Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop
            $removed += $n
        } catch {
            Write-Warn "could not remove $p : $($_.Exception.Message)"
        }
    }
    return $removed
}

<#
.SYNOPSIS
    Lay a release layout into $DestDir: an install, or an upgrade that replaces only what Agentmaster ships -
    and that puts the previous version back when the new one cannot be laid in.
.DESCRIPTION
    An upgrade first MOVES what the OLD install shipped (its manifest, else the legacy list) and every name the
    NEW layout ships into a set-aside folder inside $DestDir ($script:AmSetAsidePrefix + 8 hex digits: the
    same volume, so each move is a rename - and a running image can be renamed, unlike deleted), then copies
    the layout. When a move or the copy fails, what the copy laid down is removed and the set-aside entries
    are moved back (Restore-AmSetAside): the folder holds the previous version again, so a caller can start
    it. Until 2026-10-21 the old files were deleted before the copy, and a copy that failed half way left a
    folder with neither version - the in-app update then had nothing to start. The set-aside folder is
    deleted on success; one that could not be deleted (a file in it still in use) goes at the next run.
    The source's own profile data, pointer and decision files never travel (the destination keeps its own);
    preserved user state in the destination is never touched; anything else a user keeps in the folder
    stays. Every copied file loses the "downloaded from the Internet" mark (the Zone.Identifier stream a
    browser and Explorer's zip extraction attach, which Copy-Item carries): an installed program's files
    carry none, and a marked helper started through the shell (elevate-shim.exe) would meet a SmartScreen
    prompt of its own (measured in a test desktop on 2026-10-21: 60 of 63 installed files kept the mark).
    The user's own files in the folder keep theirs.
.PARAMETER SourceDir
    The extracted layout (the folder holding Agentmaster.exe).
.PARAMETER DestDir
    The install folder (created when missing).
.NOTES
    Throws when a move or the copy fails - after the previous version was put back - or when the result holds
    no Agentmaster.exe. When even the put-back fails, the error names the set-aside folder that still holds
    the previous version's files. Clearing a mark is best effort and never fails the install.
#>
function Install-AmLayout {
    param([string]$SourceDir, [string]$DestDir)
    New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    # A set-aside folder an earlier run could not delete (a file in it was still in use) is ours: it goes now.
    foreach ($stale in @(Get-ChildItem -LiteralPath $DestDir -Directory -Force -ErrorAction SilentlyContinue |
                Where-Object { $_.Name.StartsWith($script:AmSetAsidePrefix, [StringComparison]::OrdinalIgnoreCase) })) {
        Remove-Item -LiteralPath $stale.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
    $old = @(Get-AmShippedNames -Dir $DestDir)
    $sources = @(Get-ChildItem -LiteralPath $SourceDir -Force | Where-Object { $script:AmPreservedNames -notcontains $_.Name -and $_.Name -notlike '*.tmp' })
    $new = @($sources | ForEach-Object { $_.Name })
    # What the upgrade replaces: every name that exists, the old install's and the new layout's alike, never
    # preserved user state. A name the new layout lays down that existed before is always in this list, so
    # Restore-AmSetAside may remove every laid-down name: none of them is an old file left in place.
    $replace = @(@($old) + @($new) | Select-Object -Unique | Where-Object {
            (Test-SafeManifestName $_) -and $script:AmPreservedNames -notcontains $_ -and (Test-Path -LiteralPath (Join-Path $DestDir $_))
        })
    $aside = Join-Path $DestDir ($script:AmSetAsidePrefix + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $moved = New-Object System.Collections.Generic.List[string]
    $laid = New-Object System.Collections.Generic.List[string]
    try {
        if ($replace.Count -gt 0) { New-Item -ItemType Directory -Force -Path $aside | Out-Null }
        foreach ($n in $replace) {
            Move-Item -LiteralPath (Join-Path $DestDir $n) -Destination (Join-Path $aside $n) -Force -ErrorAction Stop
            $moved.Add($n)
        }
        foreach ($entry in $sources) {
            $target = Join-Path $DestDir $entry.Name
            # Recorded BEFORE the copy: a copy that fails half way leaves part of the entry behind.
            $laid.Add($entry.Name)
            Copy-Item -LiteralPath $entry.FullName -Destination $target -Recurse -Force -ErrorAction Stop
            # Only what this copy laid down: the target itself when it is a file, its whole tree when a folder.
            $copied = if (Test-Path -LiteralPath $target -PathType Container) { @(Get-ChildItem -LiteralPath $target -Recurse -File -Force -ErrorAction SilentlyContinue) } else { @(Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue) }
            $copied | Where-Object { $_ } | Unblock-File -ErrorAction SilentlyContinue
        }
        if (-not (Test-Path -LiteralPath (Join-Path $DestDir 'Agentmaster.exe'))) {
            throw "The installed folder holds no Agentmaster.exe: $DestDir"
        }
    } catch {
        $failure = $_
        Write-Warn "the new version could not be laid in ($($failure.Exception.Message)); putting the previous version back"
        if (-not (Restore-AmSetAside -DestDir $DestDir -AsideDir $aside -Laid $laid.ToArray() -Moved $moved.ToArray())) {
            throw "The new version could not be laid in ($($failure.Exception.Message)), and the previous version could not be put back completely: its files are in $aside."
        }
        throw $failure
    }
    if (Test-Path -LiteralPath $aside) { Remove-Item -LiteralPath $aside -Recurse -Force -ErrorAction SilentlyContinue }
}

<#
.SYNOPSIS
    Undo a failed Install-AmLayout: remove what its copy laid down, move the set-aside entries back, and delete
    the set-aside folder once it is empty.
.PARAMETER DestDir
    The install folder.
.PARAMETER AsideDir
    The set-aside folder inside it.
.PARAMETER Laid
    The names the copy had started to lay down (each existed before only when it was moved aside first).
.PARAMETER Moved
    The names that were moved into $AsideDir.
.OUTPUTS
    [bool] true when every set-aside entry is back in $DestDir (the set-aside folder is then removed).
.NOTES
    Never throws: a step that fails is reported and the others go on; the set-aside folder is kept while it
    still holds a previous file.
#>
function Restore-AmSetAside {
    param([string]$DestDir, [string]$AsideDir, [string[]]$Laid = @(), [string[]]$Moved = @())
    foreach ($n in $Laid) {
        $p = Join-Path $DestDir $n
        if (-not (Test-Path -LiteralPath $p)) { continue }
        try { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop } catch { Write-Warn "could not remove the partly copied $p : $($_.Exception.Message)" }
    }
    $all = $true
    foreach ($n in $Moved) {
        try {
            Move-Item -LiteralPath (Join-Path $AsideDir $n) -Destination (Join-Path $DestDir $n) -Force -ErrorAction Stop
        } catch {
            $all = $false
            Write-Warn "could not put $n back from $AsideDir : $($_.Exception.Message)"
        }
    }
    if ($all) {
        if (Test-Path -LiteralPath $AsideDir) { Remove-Item -LiteralPath $AsideDir -Recurse -Force -ErrorAction SilentlyContinue }
        Write-Ok 'the previous version is back in place'
    }
    return $all
}

<#
.SYNOPSIS
    The app exe inside a folder (Agentmaster.exe, or a pre-rename WindowsTerminal.exe), or $null.
.PARAMETER DirPath
    The folder.
.OUTPUTS
    [string] or $null.
#>
function Get-AppExe {
    param($DirPath)
    foreach ($leaf in 'Agentmaster.exe', 'WindowsTerminal.exe') {
        $p = Join-Path $DirPath $leaf
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

<#
.SYNOPSIS
    The layout folder inside an extracted release zip: the agentmaster-<ver> (or pre-rename terminal-<ver>)
    wrapper, else the extraction folder itself (a flat zip).
.PARAMETER ExtractDir
    Where the zip was expanded.
.OUTPUTS
    [string] the folder holding the app exe.
.NOTES
    Throws when the zip holds no Agentmaster build.
#>
function Get-AmLayoutRoot {
    param([string]$ExtractDir)
    $src = Get-ChildItem -LiteralPath $ExtractDir -Directory | Where-Object { $_.Name -like 'agentmaster-*' -or $_.Name -like 'terminal-*' } | Select-Object -First 1
    $root = if ($src) { $src.FullName } else { $ExtractDir }
    if (-not (Get-AppExe $root)) { throw 'The downloaded zip does not look like an Agentmaster portable build (no app exe inside).' }
    return $root
}

# ---- the exe-side decision and pointer files ------------------------------------------------------------
<#
.SYNOPSIS
    The keyword or folder of <Dir>\install.path: 'installed', 'machine', 'portable', an absolute folder (a
    launcher stub), or '' when there is none (PortableInstall.h DecodeInstallDecision).
.PARAMETER Dir
    The folder holding the app exe.
.OUTPUTS
    [string]
#>
function Get-AmInstallDecision {
    param([string]$Dir)
    $path = Resolve-AmPath (Join-Path $Dir 'install.path')
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -ieq 'here') { return 'installed' }
        if ($t -ieq 'installed' -or $t -ieq 'machine' -or $t -ieq 'portable') { return $t.ToLowerInvariant() }
        if ($t -match '^[A-Za-z]:[\\/]' -or $t.StartsWith('\\')) { return $t }
        return ''
    }
    return ''
}

<#
.SYNOPSIS
    Write <Dir>\install.path (the same header PortableInstall.h writes), atomically.
.PARAMETER Dir
    The folder holding the app exe.
.PARAMETER Value
    'installed', 'machine', 'portable' or an absolute folder.
.NOTES
    Throws when the folder refuses the write.
#>
function Set-AmInstallDecision {
    param([string]$Dir, [string]$Value)
    $text = "# Agentmaster install decision for THIS exe folder (PortableInstall.h):`n" +
            "#   installed  = this folder is the install (registered: shortcuts, right-click menu, Apps & Features)`n" +
            "#   machine    = this folder is the install for ALL users of this PC (registered machine-wide)`n" +
            "#   portable   = stay portable, run from here, never ask again`n" +
            "#   <dir>      = installed to <dir>; this folder only hands launches off to it`n" +
            "$Value`n"
    $path = Resolve-AmPath (Join-Path $Dir 'install.path')
    [IO.File]::WriteAllText("$path.tmp", $text, (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
}

<#
.SYNOPSIS
    The profile folder a per-user install uses: the folder the MSIX RELEASE package's own profile choice
    names (the Agentmaster_56k4f06dsfp9r= line of ~\.agentmaster.profiles), else the Default profile
    ~\.agentmaster - so a switch between the package and a per-user copy keeps every session.
.OUTPUTS
    [string]
#>
function Get-AmUserProfileDir {
    $map = Join-Path $env:USERPROFILE '.agentmaster.profiles'
    if (Test-Path -LiteralPath $map) {
        foreach ($line in [IO.File]::ReadAllLines($map)) {
            if ($line -match '^Agentmaster_56k4f06dsfp9r=(.+)$') { return $Matches[1].Trim() }
        }
    }
    return (Join-Path $env:USERPROFILE '.agentmaster')
}

<#
.SYNOPSIS
    Write <Dir>\profile.path (absolute), unless the folder already has one (an upgrade keeps the user's
    choice).
.PARAMETER Dir
    The folder holding the app exe.
.PARAMETER ProfileDir
    The profile folder.
.OUTPUTS
    [bool] true when a pointer was written.
#>
function Set-AmProfilePointer {
    param([string]$Dir, [string]$ProfileDir)
    $path = Resolve-AmPath (Join-Path $Dir 'profile.path')
    if (Test-Path -LiteralPath $path) { return $false }
    $text = "# Agentmaster profile pointer: the profile folder THIS exe location uses.`n" +
            "# A relative line resolves against the exe's folder (a moved portable keeps working).`n" +
            "$ProfileDir`n"
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))
    New-Item -ItemType Directory -Force -Path $ProfileDir | Out-Null
    return $true
}

<#
.SYNOPSIS
    True when THIS process can create a file in $Dir (a hidden probe, deleted at once).
.PARAMETER Dir
    The folder (it must exist).
.OUTPUTS
    [bool]
#>
function Test-AmDirWritable {
    param([string]$Dir)
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir)) { return $false }
    $probe = Resolve-AmPath (Join-Path $Dir (".am-write-probe-" + [guid]::NewGuid().ToString('N') + ".tmp"))
    try {
        [IO.File]::WriteAllText($probe, '')
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        return $true
    } catch {
        return $false
    }
}

# ---- registration (shortcuts, verbs, Apps & Features, PATH) ---------------------------------------------
<#
.SYNOPSIS
    Where a registration of $Scope lives: the Apps & Features key, the Classes root of the verbs, the
    environment key holding Path, and the Start-menu / desktop folders.
.DESCRIPTION
    The all-users Apps & Features key is named AgentmasterAllUsers, NOT Agentmaster like the per-user one:
    Windows lists an Uninstall key NAME once, and with both installs present the HKLM entry hid the user's
    own - Settings -> Installed apps showed only "Agentmaster (all users)", so the per-user copy could not be
    uninstalled from there (found in a test desktop on 2026-10-21). LegacyUninstallKey is where an all-users
    copy registered before that day keeps its entry: read for detection, replaced at its next registration.
.PARAMETER Scope
    'User' (HKCU, the user's own folders) or 'Machine' (HKLM, the common Start menu and the public desktop).
.OUTPUTS
    [hashtable] UninstallKey, LegacyUninstallKey ('' for User), ClassesRoot, EnvKey, StartMenu, Desktop,
    Broadcast.
.NOTES
    A test passes its own hashtable of the same shape (scratch keys and folders) instead.
#>
function Get-AmRegistrationTargets {
    param([ValidateSet('User', 'Machine')][string]$Scope)
    if ($Scope -eq 'Machine') {
        return @{
            UninstallKey       = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AgentmasterAllUsers'
            LegacyUninstallKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster'
            ClassesRoot        = 'HKLM:\Software\Classes'
            EnvKey             = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
            StartMenu          = [Environment]::GetFolderPath('CommonPrograms')
            Desktop            = [Environment]::GetFolderPath('CommonDesktopDirectory')
            Broadcast          = $true
        }
    }
    return @{
        UninstallKey       = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster'
        LegacyUninstallKey = ''
        ClassesRoot        = 'HKCU:\Software\Classes'
        EnvKey             = 'HKCU:\Environment'
        StartMenu          = [Environment]::GetFolderPath('Programs')
        Desktop            = [Environment]::GetFolderPath('DesktopDirectory')
        Broadcast          = $true
    }
}

<#
.SYNOPSIS
    Tell running shells and Explorer that the environment changed (what setx does).
.NOTES
    Never throws: a failure only means new shells pick the PATH up after a sign-out.
#>
function Send-AmEnvironmentChange {
    try {
        $sig = '[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);'
        $u32 = Add-Type -MemberDefinition $sig -Name 'AmEnvBroadcast' -Namespace 'Agentmaster' -PassThru -ErrorAction Stop
        [UIntPtr]$r = [UIntPtr]::Zero
        $null = $u32::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 3000, [ref]$r)
    } catch {
        Write-Info "environment change not broadcast: $($_.Exception.Message)"
    }
}

<#
.SYNOPSIS
    A version as Apps & Features shows it, in the app's own format (PortableInstall.h FormatVersion):
    major.minor.patch, and the fourth part only when it is not 0.
.DESCRIPTION
    A copy registered by a script then reads the same as one registered by the app, and the app's startup
    refresh (RefreshInstalledRegistration), which compares this text, never re-stamps a per-user entry over
    a mere formatting difference ("0.8.2.0" against "0.8.2").
.PARAMETER Version
    The version text ("0.8.2", "0.8.2.0", "v0.8.2-prerelease"); text that is not a version is returned as is.
.OUTPUTS
    [string]
#>
function Format-AmVersion {
    param([string]$Version)
    $num = (("$Version".Trim() -replace '^[vV]', '') -split '-')[0]
    try { $v = [version]$num } catch { return $Version }
    $text = '{0}.{1}.{2}' -f $v.Major, $v.Minor, [Math]::Max(0, $v.Build)
    if ($v.Revision -gt 0) { $text += ".$($v.Revision)" }
    return $text
}

<#
.SYNOPSIS
    The name an install of $Scope shows in the Start menu, on the desktop and in Apps & Features.
.DESCRIPTION
    An all-users copy and a user's own copy can live side by side (a user whose all-users copy could not be
    updated installs the new version just for themselves, for example), and both registrations used to say
    "Agentmaster": two identical Start-menu entries, desktop icons and Apps & Features entries, with no way to
    tell which one updates without administrator rights. The all-users install is named for what it is.
.PARAMETER Scope
    'User' or 'Machine'.
.OUTPUTS
    [string] 'Agentmaster' (User) or 'Agentmaster (all users)' (Machine).
#>
function Get-AmRegistrationName {
    param([ValidateSet('User', 'Machine')][string]$Scope)
    if ($Scope -eq 'Machine') { return 'Agentmaster (all users)' }
    return 'Agentmaster'
}

<#
.SYNOPSIS
    True when the shortcut $Lnk exists and its target lies inside $Dir.
.PARAMETER Lnk
    The .lnk file.
.PARAMETER Dir
    The folder its target must lie in.
.OUTPUTS
    [bool]
.NOTES
    Never throws: a shortcut that cannot be read counts as pointing elsewhere (it is then left alone).
#>
function Test-AmShortcutInto {
    param([string]$Lnk, [string]$Dir)
    if (-not $Lnk -or -not $Dir -or -not (Test-Path -LiteralPath $Lnk)) { return $false }
    try {
        $target = [string](New-Object -ComObject WScript.Shell).CreateShortcut($Lnk).TargetPath
        if (-not $target) { return $false }
        $full = (Resolve-AmPath $Dir).TrimEnd('\') + '\'
        return (([IO.Path]::GetFullPath($target)).TrimEnd('\') + '\').StartsWith($full, [StringComparison]::OrdinalIgnoreCase)
    } catch {
        return $false
    }
}

<#
.SYNOPSIS
    Register an installed copy: Start-menu + desktop shortcuts, the "Open in Agentmaster" verbs on folders,
    folder backgrounds and drives, an Apps & Features entry, and the folder on PATH.
.DESCRIPTION
    The same shape PortableInstall.h RegisterInstall writes (keys, values, names). The shortcuts carry no
    AppUserModelID: the script cannot compute the app's per-path formula (the app's own installer stamps it).
    An all-users copy's shortcuts and Apps & Features entry are named "Agentmaster (all users)"
    (Get-AmRegistrationName); the shortcuts an all-users copy registered before that name (Agentmaster.lnk in
    the common folders, pointing into $Dir) are replaced by the renamed ones.
    Where both installs exist, Windows puts the machine PATH before the user PATH, so a shell finds the
    all-users copy's agentmaster-cli.exe first; the "Open in Agentmaster" verbs of the user's own copy win
    (HKCU\Software\Classes overrides HKLM\Software\Classes).
.PARAMETER Scope
    'User' or 'Machine' (see Get-AmRegistrationTargets).
.PARAMETER Dir
    The install folder.
.PARAMETER Version
    The installed version; Apps & Features shows it as Format-AmVersion writes it.
.PARAMETER Targets
    Overrides Get-AmRegistrationTargets (tests).
.OUTPUTS
    [string[]] notes for the steps that failed (empty when all succeeded).
#>
function Register-AmInstall {
    param([ValidateSet('User', 'Machine')][string]$Scope, [string]$Dir, [string]$Version, [hashtable]$Targets)
    if (-not $Targets) { $Targets = Get-AmRegistrationTargets -Scope $Scope }
    $notes = @()
    $exe = Join-Path $Dir 'Agentmaster.exe'
    $name = Get-AmRegistrationName -Scope $Scope
    try {
        $sh = New-Object -ComObject WScript.Shell
        foreach ($folder in @($Targets.StartMenu, $Targets.Desktop)) {
            if (-not $folder) { continue }
            New-Item -ItemType Directory -Force -Path $folder | Out-Null
            $s = $sh.CreateShortcut((Join-Path $folder "$name.lnk"))
            $s.TargetPath = $exe
            $s.WorkingDirectory = $Dir
            $s.Description = if ($Scope -eq 'Machine') { 'Agentmaster - manage your Claude Code sessions (installed for all users of this PC)' } else { 'Agentmaster - manage your Claude Code sessions' }
            $s.IconLocation = "$exe,0"
            $s.Save()
            if ($Scope -eq 'Machine') {
                # An all-users copy registered by a build of 2026-10-21 (the first with installs for all users)
                # named its shortcuts Agentmaster.lnk: the renamed one replaces it - only when it points here.
                $older = Join-Path $folder 'Agentmaster.lnk'
                if (Test-AmShortcutInto -Lnk $older -Dir $Dir) { Remove-Item -LiteralPath $older -Force -ErrorAction SilentlyContinue }
            }
        }
    } catch { $notes += "shortcuts: $($_.Exception.Message)" }
    try {
        foreach ($verb in @(@('Directory', '%1'), @('Directory\Background', '%V'), @('Drive', '%1'))) {
            $key = "$($Targets.ClassesRoot)\$($verb[0])\shell\Agentmaster"
            New-Item -Path "$key\command" -Force | Out-Null
            Set-Item -LiteralPath $key -Value 'Open in Agentmaster'
            New-ItemProperty -LiteralPath $key -Name 'Icon' -Value "`"$exe`",0" -PropertyType String -Force | Out-Null
            Set-Item -LiteralPath "$key\command" -Value "`"$exe`" -d `"$($verb[1])`""
        }
    } catch { $notes += "right-click menu: $($_.Exception.Message)" }
    try {
        $u = $Targets.UninstallKey
        New-Item -Path $u -Force | Out-Null
        $size = 0
        try { $size = [int]((Get-ChildItem -LiteralPath $Dir -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1KB) } catch { $size = 0 }
        $values = [ordered]@{
            DisplayName     = $name
            DisplayVersion  = (Format-AmVersion $Version)
            Publisher       = 'Agentmaster'
            InstallLocation = $Dir
            DisplayIcon     = $exe
            UninstallString = "`"$exe`" --uninstall-portable"
            InstallDate     = (Get-Date -Format 'yyyyMMdd')
            URLInfoAbout    = 'https://github.com/Nucs/Agentmaster/releases'
        }
        foreach ($k in $values.Keys) { New-ItemProperty -LiteralPath $u -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
        New-ItemProperty -LiteralPath $u -Name 'NoModify' -Value 1 -PropertyType DWord -Force | Out-Null
        New-ItemProperty -LiteralPath $u -Name 'NoRepair' -Value 1 -PropertyType DWord -Force | Out-Null
        if ($size -gt 0) { New-ItemProperty -LiteralPath $u -Name 'EstimatedSize' -Value $size -PropertyType DWord -Force | Out-Null }
        # An all-users copy registered before 2026-10-21 has its entry under the per-user key's name, which
        # hides a user's own entry in Settings (Get-AmRegistrationTargets): the renamed one replaces it - only
        # when it describes this folder.
        $legacy = [string]$Targets.LegacyUninstallKey
        if ($legacy -and $legacy -ne $u -and (Test-Path -LiteralPath $legacy)) {
            $legacyLoc = [string](Get-Item -LiteralPath $legacy).GetValue('InstallLocation')
            if ($legacyLoc -and ([IO.Path]::GetFullPath($legacyLoc)).TrimEnd('\') -ieq ([IO.Path]::GetFullPath($Dir)).TrimEnd('\')) {
                Remove-Item -LiteralPath $legacy -Recurse -Force
            }
        }
    } catch { $notes += "Apps & Features entry: $($_.Exception.Message)" }
    try {
        if (-not (Test-Path -LiteralPath $Targets.EnvKey)) { New-Item -Path $Targets.EnvKey -Force | Out-Null }
        $envItem = Get-Item -LiteralPath $Targets.EnvKey
        $raw = [string]$envItem.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
        $entries = @($raw -split ';' | Where-Object { $_ })
        $present = $entries | Where-Object { $_.Trim().Trim('"').TrimEnd('\') -ieq $Dir.TrimEnd('\') }
        if (-not $present) {
            $new = (@($entries) + $Dir) -join ';'
            New-ItemProperty -LiteralPath $Targets.EnvKey -Name 'Path' -Value $new -PropertyType ExpandString -Force | Out-Null
            if ($Targets.Broadcast) { Send-AmEnvironmentChange }
        }
    } catch { $notes += "PATH: $($_.Exception.Message)" }
    return $notes
}

<#
.SYNOPSIS
    Remove a registration of $Scope - only the entries that point INTO $Dir, so another install (elsewhere,
    of the other scope, or the MSIX package) keeps its own.
.PARAMETER Scope
    'User' or 'Machine'.
.PARAMETER Dir
    The install folder being removed.
.PARAMETER Targets
    Overrides Get-AmRegistrationTargets (tests).
.OUTPUTS
    [string[]] notes for the steps that failed.
#>
function Unregister-AmInstall {
    param([ValidateSet('User', 'Machine')][string]$Scope, [string]$Dir, [hashtable]$Targets)
    if (-not $Targets) { $Targets = Get-AmRegistrationTargets -Scope $Scope }
    $notes = @()
    $full = (Resolve-AmPath $Dir).TrimEnd('\')
    $inside = { param($p) $p -and (([IO.Path]::GetFullPath($p)).TrimEnd('\') + '\').StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) }
    try {
        $sh = New-Object -ComObject WScript.Shell
        foreach ($folder in @($Targets.StartMenu, $Targets.Desktop)) {
            if (-not $folder) { continue }
            # Both scopes' names in both scopes' folders: each is removed only when it points into $Dir.
            foreach ($name in 'Agentmaster.lnk', 'Agentmaster (all users).lnk', 'Agentmaster (Portable).lnk') {
                $lnk = Join-Path $folder $name
                if (-not (Test-Path -LiteralPath $lnk)) { continue }
                $target = ''
                try { $target = $sh.CreateShortcut($lnk).TargetPath } catch { $target = '' }
                if (& $inside $target) { Remove-Item -LiteralPath $lnk -Force; Write-Ok "removed $lnk" }
                else { Write-Info "kept $lnk (points elsewhere: $target)" }
            }
        }
    } catch { $notes += "shortcuts: $($_.Exception.Message)" }
    try {
        foreach ($base in 'Directory', 'Directory\Background', 'Drive') {
            $key = "$($Targets.ClassesRoot)\$base\shell\Agentmaster"
            if (-not (Test-Path -LiteralPath $key)) { continue }
            $cmd = ''
            try { $cmd = [string](Get-Item -LiteralPath "$key\command" -ErrorAction Stop).GetValue('') } catch { $cmd = '' }
            if ($cmd -and $cmd.IndexOf($full, [StringComparison]::OrdinalIgnoreCase) -ge 0) { Remove-Item -LiteralPath $key -Recurse -Force; Write-Ok "removed $key" }
            else { Write-Info "kept $key (points elsewhere)" }
        }
    } catch { $notes += "right-click menu: $($_.Exception.Message)" }
    try {
        # The scope's entry, and for the all-users scope also the one registered under the old name (before
        # 2026-10-21 it shared the per-user key's name; Get-AmRegistrationTargets).
        foreach ($uninstallKey in @($Targets.UninstallKey, $Targets.LegacyUninstallKey)) {
            if (-not $uninstallKey -or -not (Test-Path -LiteralPath $uninstallKey)) { continue }
            $loc = [string](Get-Item -LiteralPath $uninstallKey).GetValue('InstallLocation')
            if (-not $loc -or ([IO.Path]::GetFullPath($loc)).TrimEnd('\') -ieq $full) { Remove-Item -LiteralPath $uninstallKey -Recurse -Force; Write-Ok 'removed the Apps & Features entry' }
            else { Write-Info "kept the Apps & Features entry (it describes another install at $loc)" }
        }
    } catch { $notes += "Apps & Features entry: $($_.Exception.Message)" }
    try {
        if (Test-Path -LiteralPath $Targets.EnvKey) {
            $envItem = Get-Item -LiteralPath $Targets.EnvKey
            $raw = [string]$envItem.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
            if ($raw) {
                $kept = @($raw -split ';' | Where-Object { $_ -and ($_.Trim().Trim('"').TrimEnd('\') -ine $full) })
                $new = $kept -join ';'
                if ($new -ne $raw) {
                    New-ItemProperty -LiteralPath $Targets.EnvKey -Name 'Path' -Value $new -PropertyType ExpandString -Force | Out-Null
                    if ($Targets.Broadcast) { Send-AmEnvironmentChange }
                    Write-Ok 'removed the PATH entry'
                }
            }
        }
    } catch { $notes += "PATH: $($_.Exception.Message)" }
    return $notes
}

<#
.SYNOPSIS
    Uninstall a copy: close it, remove its registration of $Scope (only entries pointing into $Dir), the
    names Agentmaster shipped, a set-aside folder an interrupted upgrade left, and its install.path - and
    NOTHING else: settings\, profile\, profile.path, a user's own files and every profile folder stay. The
    folder itself goes only when it is left empty.
.PARAMETER Scope
    'User' or 'Machine'.
.PARAMETER Dir
    The install folder.
.PARAMETER Targets
    Overrides Get-AmRegistrationTargets (tests).
.PARAMETER Force
    Close other users' instances of an all-users copy too.
.OUTPUTS
    [string[]] the names left in the folder (empty when the folder was removed).
#>
function Uninstall-AmCopy {
    param([ValidateSet('User', 'Machine')][string]$Scope, [string]$Dir, [hashtable]$Targets, [switch]$Force)
    $full = (Resolve-AmPath $Dir).TrimEnd('\')
    Stop-ProcessesUnder -DirPath $full -AllowOtherUsers:$Force
    Write-Step 'Removing shortcuts, the right-click menu, the Apps & Features entry and the PATH entry'
    foreach ($n in (Unregister-AmInstall -Scope $Scope -Dir $full -Targets $Targets)) { Write-Warn $n }
    Write-Step "Removing the files Agentmaster installed in $full"
    $null = Remove-AmEntries -Dir $full -Names @(Get-AmShippedNames -Dir $full)
    # A set-aside folder an interrupted upgrade left behind (Install-AmLayout) holds only Agentmaster's own files.
    foreach ($stale in @(Get-ChildItem -LiteralPath $full -Directory -Force -ErrorAction SilentlyContinue |
                Where-Object { $_.Name.StartsWith($script:AmSetAsidePrefix, [StringComparison]::OrdinalIgnoreCase) })) {
        Remove-Item -LiteralPath $stale.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
    $decision = Join-Path $full 'install.path'
    if (Test-Path -LiteralPath $decision) { Remove-Item -LiteralPath $decision -Force -ErrorAction SilentlyContinue }
    $left = @(Get-ChildItem -LiteralPath $full -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    if ($left.Count -eq 0) {
        Remove-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
        Write-Ok 'removed the folder'
    } else {
        Write-Ok ("kept what is not Agentmaster's own: " + ($left -join ', '))
    }
    return $left
}

# ---- doing things as the signed-in user -----------------------------------------------------------------
<#
.SYNOPSIS
    Run a program as another signed-in user (or as this one without administrator rights), through a
    one-shot scheduled task: interactive logon (it runs in that user's session, no password needed),
    limited rights.
.DESCRIPTION
    The de-escalation every install path needs: an elevated or other-account script installs the per-user
    part, and starts the app, as the user signed in to Windows. Registering a task for ANOTHER user needs
    administrator rights (or SYSTEM). The task is deleted once its program finished (-Wait) or started.
.PARAMETER UserSid
    The SID of the user to run as (the main user).
.PARAMETER FilePath
    The program.
.PARAMETER Arguments
    Its command line arguments.
.PARAMETER Wait
    Wait until the program exits and return its exit code.
.PARAMETER TimeoutSec
    How long to wait for the task to start (and, with -Wait, to finish).
.OUTPUTS
    [int] the program's exit code with -Wait; 0 when it started without -Wait.
.NOTES
    Throws when the task cannot be registered or started (no rights, the scheduler disabled), or does not
    start or finish within -TimeoutSec.
#>
function Invoke-AmAsUser {
    param([string]$UserSid, [string]$FilePath, [string]$Arguments = '', [switch]$Wait, [int]$TimeoutSec = 900)
    $name = 'Agentmaster-as-user-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
    $action = if ($Arguments) { New-ScheduledTaskAction -Execute $FilePath -Argument $Arguments } else { New-ScheduledTaskAction -Execute $FilePath }
    $principal = New-ScheduledTaskPrincipal -UserId $UserSid -LogonType Interactive -RunLevel Limited
    $limit = if ($Wait) { New-TimeSpan -Seconds ([Math]::Max(60, $TimeoutSec)) } else { [TimeSpan]::Zero }
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit $limit -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null
    try {
        Start-ScheduledTask -TaskName $name -ErrorAction Stop
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        $started = $false
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 400
            $task = Get-ScheduledTask -TaskName $name -ErrorAction Stop
            $info = Get-ScheduledTaskInfo -TaskName $name -ErrorAction Stop
            $ran = $info.LastTaskResult -ne 267011   # SCHED_S_TASK_HAS_NOT_RUN
            if ($task.State -eq 'Running' -or $ran) { $started = $true }
            if (-not $Wait -and $started) { return 0 }
            if ($Wait -and $ran -and $task.State -ne 'Running') { return [int]$info.LastTaskResult }
        }
        if (-not $started) { throw "The task to start $FilePath as the signed-in user did not start within $TimeoutSec s." }
        throw "$FilePath did not finish within $TimeoutSec s."
    } finally {
        Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    }
}

<#
.SYNOPSIS
    Start the Agentmaster app for the signed-in user WITHOUT administrator rights.
.DESCRIPTION
    When this process is the signed-in user and not elevated, a plain Start-Process. Otherwise (elevated, or
    another account, or SYSTEM) a one-shot task for the signed-in user starts it through a SHORT launcher
    that exits at once - explorer.exe with the app (the user's own Explorer starts it, so the app is neither
    elevated nor a child of the task), or, when there are arguments, a hidden powershell that Start-Process-es
    it - and the task is deleted only after that launcher finished. When even that is impossible (another
    account without administrator rights), it says how to start the app instead.
.PARAMETER Exe
    The app exe.
.PARAMETER Identity
    Get-AmIdentity's result.
.PARAMETER Arguments
    The app's command line (verbatim; may be empty).
.OUTPUTS
    [bool] true when the start was handed over.
#>
function Start-AmAppAsUser {
    param([string]$Exe, $Identity, [string]$Arguments = '')
    if (-not (Test-Path -LiteralPath $Exe)) { Write-Warn "app exe not found: $Exe"; return $false }
    if ($Identity.IsMainUser -and -not $Identity.Elevated) {
        if ($Arguments) { Start-Process -FilePath $Exe -ArgumentList $Arguments } else { Start-Process -FilePath $Exe }
        return $true
    }
    if (-not $Identity.MainUserSid -or -not ($Identity.Elevated -or $Identity.IsSystem)) {
        Write-Warn "Start Agentmaster from the Start menu as $($Identity.MainUserName): $Exe"
        return $false
    }
    try {
        if ($Arguments) {
            # The command line rides as base64 of its UTF-16LE bytes: no quoting layer can damage it.
            $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Arguments))
            $cmd = "Start-Process -FilePath '" + $Exe.Replace("'", "''") + "' -ArgumentList ([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('$b64')))"
            $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
            $null = Invoke-AmAsUser -UserSid $Identity.MainUserSid -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
                        -Arguments "-NoProfile -WindowStyle Hidden -EncodedCommand $enc" -Wait -TimeoutSec 60
        } else {
            # explorer.exe's own exit code is 1 even when it started the program: only "finished" counts.
            $null = Invoke-AmAsUser -UserSid $Identity.MainUserSid -FilePath "$env:SystemRoot\explorer.exe" -Arguments "`"$Exe`"" -Wait -TimeoutSec 60
        }
        return $true
    } catch {
        Write-Warn "could not start Agentmaster as $($Identity.MainUserName): $($_.Exception.Message)"
        return $false
    }
}

<#
.SYNOPSIS
    The text of the named functions (this core's), for a phase script another process runs.
.PARAMETER Names
    The names of functions defined in this session.
.OUTPUTS
    [string] "function X { ... }" blocks, in order.
.NOTES
    Throws when a name is not a defined function.
#>
function Export-AmFunctions {
    param([string[]]$Names)
    $sb = New-Object System.Text.StringBuilder
    foreach ($n in $Names) {
        $f = Get-Item -LiteralPath "function:$n" -ErrorAction Stop
        $null = $sb.AppendLine("function $n {")
        $null = $sb.AppendLine($f.ScriptBlock.ToString())
        $null = $sb.AppendLine('}')
    }
    return $sb.ToString()
}

<#
.SYNOPSIS
    Run a piece of PowerShell as the signed-in user (a one-shot task), with this core loaded, and stream its
    output back here.
.DESCRIPTION
    The per-user half of an install that an elevated or other-account script makes: the phase script is
    written into a folder the signed-in user owns (inside their profile, so its ACL is theirs), runs hidden
    in their session with limited rights, records everything through Start-Transcript, and this process
    prints that record when it ends.
.PARAMETER Identity
    Get-AmIdentity's result (MainUserSid + MainUserProfile are required).
.PARAMETER Body
    The phase's own commands (after the core's functions); they may use $PhaseDir.
.PARAMETER Files
    Paths copied into the phase folder first ($PhaseDir), e.g. the downloaded zip.
.OUTPUTS
    [pscustomobject] Code (the phase's exit code; 0 = success) and Result (its result.txt, '' when none).
.NOTES
    Throws when there is no signed-in user to run as, or the task cannot run.
#>
function Invoke-AmPhaseAsUser {
    param($Identity, [string]$Body, [string[]]$Files = @())
    if (-not $Identity.MainUserSid -or -not $Identity.MainUserProfile) { throw 'Nobody is signed in to this PC to install for.' }
    $phaseDir = Join-Path $Identity.MainUserProfile ('AppData\Local\Temp\Agentmaster-install-' + [guid]::NewGuid().ToString('N').Substring(0, 12))
    New-Item -ItemType Directory -Force -Path $phaseDir | Out-Null
    foreach ($f in $Files) { Copy-Item -LiteralPath $f -Destination $phaseDir -Recurse -Force }
    $ps1 = Write-AmPhaseScript -PhaseDir $phaseDir -Body $Body
    Write-Info "running the per-user part as $($Identity.MainUserName)"
    $code = Invoke-AmAsUser -UserSid $Identity.MainUserSid -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
                -Arguments "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ps1`"" -Wait
    $result = Read-AmPhaseRecord -PhaseDir $phaseDir
    Remove-Item -LiteralPath $phaseDir -Recurse -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ Code = $code; Result = $result }
}

# ---- the two copy installs -------------------------------------------------------------------------------
<#
.SYNOPSIS
    Copy a profile folder's data into another profile, never overwriting a file the target already holds
    (the app's MigrateProfileData rule, ProfileBootstrap.h).
.DESCRIPTION
    Left out at the top level, like the app: locks\ (machine tooling), shim\ and bridge.json (regenerated with
    live paths at every start), bin\ (an install's binaries, not data) and *.tmp.
.PARAMETER From
    The profile folder to copy from.
.PARAMETER To
    The profile folder to copy into (created as needed).
.OUTPUTS
    [int] the number of files copied.
.NOTES
    Never throws for one file: a file that cannot be copied is reported and the rest goes on.
#>
function Copy-AmProfileData {
    param([string]$From, [string]$To)
    if (-not $From -or -not $To -or -not (Test-Path -LiteralPath $From)) { return 0 }
    $fromFull = (Resolve-AmPath $From).TrimEnd('\')
    $toFull = (Resolve-AmPath $To).TrimEnd('\')
    if ($fromFull -ieq $toFull) { return 0 }
    $copied = 0
    foreach ($f in @(Get-ChildItem -LiteralPath $fromFull -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        $rel = $f.FullName.Substring($fromFull.Length + 1)
        $top = ($rel -split '\\')[0]
        if (@('locks', 'shim', 'bin', 'bridge.json') -contains $top -or $top -like '*.tmp') { continue }
        $target = Join-Path $toFull $rel
        if (Test-Path -LiteralPath $target) { continue }
        try {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-Item -LiteralPath $f.FullName -Destination $target -ErrorAction Stop
            $copied++
        } catch {
            Write-Warn "could not copy $rel into $toFull : $($_.Exception.Message)"
        }
    }
    return $copied
}

<#
.SYNOPSIS
    Retire the Start-menu shortcut "Agentmaster (Portable)" that Install-Agentmaster.ps1 v0.7.10 and older
    made, once the per-user install it led to is registered under the standard name.
.DESCRIPTION
    Removed only when it points into $InstallDir, or into a launcher stub folder whose install.path names
    $InstallDir (what the app's first-launch question makes of the old script's folder when it installs
    elsewhere): a shortcut to any other copy stays. Until 2026-10-21 it was removed whatever it pointed at -
    also after an update that kept a portable folder portable, which then had no Start-menu entry at all.
.PARAMETER InstallDir
    The registered per-user install.
.PARAMETER ProgramsDir
    The Start-menu Programs folder; '' = the current user's (tests pass a scratch one).
.OUTPUTS
    [bool] true when the shortcut was removed.
.NOTES
    Never throws: a shortcut that cannot be read is left alone.
#>
function Remove-AmLegacyScriptShortcut {
    param([string]$InstallDir, [string]$ProgramsDir = '')
    if (-not $ProgramsDir) { $ProgramsDir = [Environment]::GetFolderPath('Programs') }
    $lnk = Join-Path $ProgramsDir 'Agentmaster (Portable).lnk'
    if (-not $InstallDir -or -not (Test-Path -LiteralPath $lnk)) { return $false }
    $retire = Test-AmShortcutInto -Lnk $lnk -Dir $InstallDir
    if (-not $retire) {
        try {
            $target = [string](New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath
            if ($target) {
                $stub = Get-AmInstallDecision -Dir (Split-Path -Parent $target)
                $retire = [bool]$stub -and ($stub.TrimEnd('\') -ieq (Resolve-AmPath $InstallDir).TrimEnd('\'))
            }
        } catch {
            $retire = $false
        }
    }
    if (-not $retire) { return $false }
    Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue
    Write-Info 'retired the old "Agentmaster (Portable)" Start-menu shortcut'
    return $true
}

<#
.SYNOPSIS
    Install (or upgrade) a PER-USER copy for the user running this code: the layout, install.path =
    installed (unless the folder was deliberately kept portable), profile.path -> $ProfileDir, and the user
    registration. No administrator rights.
.DESCRIPTION
    A folder that never chose a profile (the old script's folder, run with "Ask me later") kept its data in
    <folder>\profile; the installed copy uses $ProfileDir, so that data is copied into it first, never over a
    file $ProfileDir already holds (Copy-AmProfileData). <folder>\profile itself stays where it is.
.PARAMETER LayoutDir
    The extracted layout (holding Agentmaster.exe).
.PARAMETER DestDir
    The install folder.
.PARAMETER Version
    The installed version (Apps & Features).
.PARAMETER Targets
    Overrides the registration targets (tests).
.PARAMETER ProfileDir
    The profile the installed copy uses; '' = Get-AmUserProfileDir (the MSIX package's choice, else the
    Default profile). The in-app updater passes the profile the running app used, so a switch from an
    all-users copy or the package keeps exactly that data.
.OUTPUTS
    [bool] true when the folder became (or stayed) a registered install; false when it was kept portable by
    choice and only its files were updated.
.NOTES
    Throws when the folder is not writable without administrator rights or the copy fails (Install-AmLayout
    then puts the previous version back).
#>
function Install-AmUserCopy {
    param([string]$LayoutDir, [string]$DestDir, [string]$Version, [hashtable]$Targets, [string]$ProfileDir = '')
    $parent = $DestDir
    while ($parent -and -not (Test-Path -LiteralPath $parent)) { $parent = Split-Path -Parent $parent }
    if (-not (Test-AmDirWritable $parent)) {
        throw "$DestDir needs administrator rights to write. Install for all users instead, or pick a folder of your own."
    }
    $keptPortable = (Get-AmInstallDecision -Dir $DestDir) -eq 'portable'
    Stop-ProcessesUnder -DirPath $DestDir
    Write-Step "Installing Agentmaster $Version into $DestDir"
    Install-AmLayout -SourceDir $LayoutDir -DestDir $DestDir
    if ($keptPortable) {
        Write-Info 'this folder was kept portable by choice: only its files were updated'
        return $false
    }
    Set-AmInstallDecision -Dir $DestDir -Value 'installed'
    if (-not $ProfileDir) { $ProfileDir = Get-AmUserProfileDir }
    if (-not (Test-Path -LiteralPath (Join-Path $DestDir 'profile.path'))) {
        $own = Join-Path $DestDir 'profile'
        if (Test-Path -LiteralPath $own) {
            $copiedFiles = Copy-AmProfileData -From $own -To $ProfileDir
            if ($copiedFiles -gt 0) { Write-Ok "copied $copiedFiles file(s) of this folder's own profile into $ProfileDir" }
        }
    }
    if (Set-AmProfilePointer -Dir $DestDir -ProfileDir $ProfileDir) { Write-Ok ("profile: " + $ProfileDir) }
    foreach ($n in (Register-AmInstall -Scope User -Dir $DestDir -Version $Version -Targets $Targets)) { Write-Warn "registration: $n" }
    Write-Ok 'installed just for this user (no administrator rights)'
    return $true
}

<#
.SYNOPSIS
    Install (or upgrade) the ALL-USERS copy: the layout, install.path = machine, the machine registration.
    Each user keeps their own data (the app resolves a machine copy's profile in that user's home).
.PARAMETER LayoutDir
    The extracted layout.
.PARAMETER DestDir
    The install folder (normally <Program Files>\Agentmaster).
.PARAMETER Version
    The installed version.
.PARAMETER Targets
    Overrides the registration targets (tests).
.PARAMETER Force
    Close other users' running instances.
.NOTES
    Needs administrator rights; throws without them, when the layout predates all-users support (no
    .am-manifest: its app would not know install.path = machine), or when the copy fails.
#>
function Install-AmMachineCopy {
    param([string]$LayoutDir, [string]$DestDir, [string]$Version, [hashtable]$Targets, [switch]$Force)
    if (-not $Targets -and -not (Test-Admin)) { throw 'Installing for all users needs administrator rights.' }
    if (@(Read-AmManifest -Dir $LayoutDir).Count -eq 0) {
        throw "This release predates installs for all users (its zip has no .am-manifest). Install it just for you, or pick a newer release."
    }
    Stop-ProcessesUnder -DirPath $DestDir -AllowOtherUsers:$Force
    Write-Step "Installing Agentmaster $Version for all users into $DestDir"
    Install-AmLayout -SourceDir $LayoutDir -DestDir $DestDir
    Set-AmInstallDecision -Dir $DestDir -Value 'machine'
    foreach ($n in (Register-AmInstall -Scope Machine -Dir $DestDir -Version $Version -Targets $Targets)) { Write-Warn "registration: $n" }
    Write-Ok 'installed for all users of this PC'
}

# ---- where the installs are ------------------------------------------------------------------------------
# The release package family (the MSIX); its profile choice is what a per-user install adopts.
$script:AmReleaseFamily = 'Agentmaster_56k4f06dsfp9r'

<#
.SYNOPSIS
    The default per-user install folder of the user running this code: %USERPROFILE%\.agentmaster\bin (the
    app's own default, PortableInstall.h DefaultUserInstallDir).
.OUTPUTS
    [string]
#>
function Get-AmDefaultUserInstallDir { return (Join-Path $env:USERPROFILE '.agentmaster\bin') }

<#
.SYNOPSIS
    The default all-users install folder: <64-bit Program Files>\Agentmaster.
.OUTPUTS
    [string]
#>
function Get-AmDefaultMachineInstallDir {
    $pf = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
    return (Join-Path $pf 'Agentmaster')
}

<#
.SYNOPSIS
    The folder of the CURRENT user's existing per-user install, or '' when there is none.
.DESCRIPTION
    In order: the Apps & Features entry's InstallLocation (an install made by the app's first-launch
    question or by this script); then the folder older versions of Install-Agentmaster.ps1 used
    (%LOCALAPPDATA%\Programs\Agentmaster) - or, when the app later turned that folder into a launcher stub,
    the folder its install.path names.
.OUTPUTS
    [string]
#>
function Find-AmUserInstallDir {
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster'
    # Test-Path first: the key is missing on every first install, and a thrown-and-caught "not found" still
    # lands in a phase's transcript as a TerminatingError line that the user reads as a failure (seen in a
    # test desktop, 2026-10-21). The catch is left for a key that exists but cannot be read.
    if (Test-Path -LiteralPath $key) {
        try {
            $loc = [string](Get-ItemProperty -LiteralPath $key -ErrorAction Stop).InstallLocation
            if ($loc -and (Get-AppExe $loc)) { return $loc }
        } catch { $loc = '' }
    }
    $legacy = Join-Path $env:LOCALAPPDATA 'Programs\Agentmaster'
    if (Get-AppExe $legacy) {
        $d = Get-AmInstallDecision -Dir $legacy
        if ($d -and $d -ne 'installed' -and $d -ne 'portable' -and $d -ne 'machine' -and (Get-AppExe $d)) { return $d }
        return $legacy
    }
    return ''
}

<#
.SYNOPSIS
    The folder of the existing all-users install (the HKLM Apps & Features entry with a live exe), or ''.
.DESCRIPTION
    The entry's key is AgentmasterAllUsers; an all-users copy registered before 2026-10-21 still has it under
    the old name, Agentmaster (Get-AmRegistrationTargets), which is read second.
.OUTPUTS
    [string]
#>
function Find-AmMachineInstallDir {
    $t = Get-AmRegistrationTargets -Scope Machine
    foreach ($key in @($t.UninstallKey, $t.LegacyUninstallKey)) {
        # Test-Path first, like Find-AmUserInstallDir: no thrown "not found" in a phase's transcript.
        if (-not (Test-Path -LiteralPath $key)) { continue }
        try {
            $loc = [string](Get-ItemProperty -LiteralPath $key -ErrorAction Stop).InstallLocation
            if ($loc -and (Get-AppExe $loc)) { return $loc }
        } catch { $loc = '' }
    }
    return ''
}

<#
.SYNOPSIS
    The version stamped in <Dir>\.am-version, or $null.
.PARAMETER Dir
    An install folder.
.OUTPUTS
    [version] or $null.
#>
function Get-AmInstalledVersion {
    param([string]$Dir)
    $stamp = Join-Path $Dir '.am-version'
    if (-not (Test-Path -LiteralPath $stamp)) { return $null }
    try { return [version](([IO.File]::ReadAllText((Resolve-AmPath $stamp))).Trim()) } catch { return $null }
}

<#
.SYNOPSIS
    Expand a release zip into a fresh folder and return its layout root (Get-AmLayoutRoot).
.PARAMETER ZipPath
    The downloaded zip.
.PARAMETER WorkDir
    A folder to expand into (a new subfolder is made inside it).
.OUTPUTS
    [string] the layout root.
.NOTES
    Throws when the zip cannot be expanded or holds no Agentmaster build.
#>
function Expand-AmZip {
    param([string]$ZipPath, [string]$WorkDir)
    $dest = Join-Path $WorkDir ('extract-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $dest -Force
    return (Get-AmLayoutRoot $dest)
}

<#
.SYNOPSIS
    The whole per-user install for the user RUNNING this code (a script run by the signed-in user, or the
    per-user phase an elevated script hands to them): find the install folder, skip an up-to-date one,
    install the layout, retire the legacy script shortcut, optionally remove the MSIX package.
.PARAMETER ZipPath
    The release's portable zip.
.PARAMETER Version
    The release version.
.PARAMETER DestDir
    An explicit install folder; '' = the existing per-user install (Find-AmUserInstallDir), else the default.
.PARAMETER Force
    Reinstall even when the folder already holds this version (or a newer one).
.PARAMETER RemovePackage
    Remove the user's MSIX release package afterwards (the takeover: the same data stays).
.PARAMETER ResultFile
    When set, the installed exe's path is written there (how a hidden phase reports back).
.PARAMETER ProfileDir
    The profile the installed copy uses (Install-AmUserCopy); '' = Get-AmUserProfileDir.
.OUTPUTS
    [string] the installed Agentmaster.exe.
.NOTES
    Throws when the folder needs administrator rights or the install fails.
#>
function Invoke-AmUserInstall {
    param([string]$ZipPath, [string]$Version, [string]$DestDir = '', [switch]$Force, [switch]$RemovePackage, [string]$ResultFile = '', [string]$ProfileDir = '')
    $dest = $DestDir
    if (-not $dest) { $dest = Find-AmUserInstallDir }
    if (-not $dest) { $dest = Get-AmDefaultUserInstallDir }
    $cur = Get-AmInstalledVersion -Dir $dest
    $registered = (Get-AmInstallDecision -Dir $dest) -eq 'installed'
    if (-not $Force -and $cur -and $registered -and $cur -ge [version]$Version) {
        Write-Ok "Already up to date for this user (installed $cur >= release $Version) at $dest. Use -Force to reinstall."
    } else {
        $work = Join-Path $env:TEMP ('Agentmaster-install-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        $nowRegistered = $false
        try {
            $root = Expand-AmZip -ZipPath $ZipPath -WorkDir $work
            # Captured: its [bool] must not join this function's own output (the exe path).
            $nowRegistered = Install-AmUserCopy -LayoutDir $root -DestDir $dest -Version $Version -ProfileDir $ProfileDir
        } finally {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
        # Older versions of this script made "Agentmaster (Portable).lnk"; once the folder it leads to is
        # registered, the standard Agentmaster.lnk replaces it (a folder kept portable keeps its old entry).
        if ($nowRegistered) { $null = Remove-AmLegacyScriptShortcut -InstallDir $dest }
    }
    if ($RemovePackage) {
        $pkg = Get-AmPackage -FamilyName $script:AmReleaseFamily
        if ($pkg) { Remove-AmPackage -Package $pkg }
    }
    $exe = Join-Path $dest 'Agentmaster.exe'
    if ($ResultFile) { [IO.File]::WriteAllText((Resolve-AmPath $ResultFile), $exe, (New-Object Text.UTF8Encoding($false))) }
    return $exe
}

<#
.SYNOPSIS
    The whole all-users install (administrator rights): the install folder, an up-to-date check, the layout
    and the machine registration.
.PARAMETER ZipPath
    The release's portable zip.
.PARAMETER Version
    The release version.
.PARAMETER DestDir
    An explicit folder; '' = the existing all-users install, else <Program Files>\Agentmaster.
.PARAMETER Force
    Reinstall an up-to-date folder, and close other users' running instances.
.PARAMETER ResultFile
    When set, the installed exe's path is written there.
.OUTPUTS
    [string] the installed Agentmaster.exe.
.NOTES
    Throws without administrator rights, for a release that predates all-users installs, or when it fails.
#>
function Invoke-AmMachineInstall {
    param([string]$ZipPath, [string]$Version, [string]$DestDir = '', [switch]$Force, [string]$ResultFile = '')
    $dest = $DestDir
    if (-not $dest) { $dest = Find-AmMachineInstallDir }
    if (-not $dest) { $dest = Get-AmDefaultMachineInstallDir }
    $cur = Get-AmInstalledVersion -Dir $dest
    if (-not $Force -and $cur -and (Get-AmInstallDecision -Dir $dest) -eq 'machine' -and $cur -ge [version]$Version) {
        Write-Ok "Already up to date for all users (installed $cur >= release $Version) at $dest. Use -Force to reinstall."
    } else {
        $work = Join-Path $env:TEMP ('Agentmaster-install-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        try {
            $root = Expand-AmZip -ZipPath $ZipPath -WorkDir $work
            Install-AmMachineCopy -LayoutDir $root -DestDir $dest -Version $Version -Force:$Force
        } finally {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    $exe = Join-Path $dest 'Agentmaster.exe'
    if ($ResultFile) { [IO.File]::WriteAllText((Resolve-AmPath $ResultFile), $exe, (New-Object Text.UTF8Encoding($false))) }
    return $exe
}

<#
.SYNOPSIS
    The Win32 error code inside a failed command's exception chain (Start-Process wraps it), or $Default.
.PARAMETER ErrorRecord
    The caught error ($_ in a catch block).
.PARAMETER Default
    The code returned when the chain holds no Win32 error.
.OUTPUTS
    [int]
#>
function Get-AmWin32ErrorCode {
    param($ErrorRecord, [int]$Default = 0)
    $e = $null
    if ($ErrorRecord) { $e = $ErrorRecord.Exception }
    while ($e) {
        if ($e -is [System.ComponentModel.Win32Exception]) { return [int]$e.NativeErrorCode }
        $e = $e.InnerException
    }
    return $Default
}

<#
.SYNOPSIS
    Run Windows PowerShell WITH administrator rights (the "runas" verb: one UAC prompt) and wait for it.
.DESCRIPTION
    Process.Start, NOT Start-Process -Verb RunAs: Windows PowerShell 5.1's Start-Process turns the Win32
    failure into a bare InvalidOperationException that carries only a localized message (checked in a test
    desktop, 2026-10-21), while Process.Start throws a Win32Exception whose NativeErrorCode tells a declined
    prompt (1223) from a refused one (1260) - read it with Get-AmWin32ErrorCode.
.PARAMETER Arguments
    The powershell.exe command line after the exe.
.OUTPUTS
    [int] the elevated process's exit code.
.NOTES
    Throws (a method-invocation error wrapping the Win32Exception) when the elevation is declined or
    refused, or the process cannot start.
#>
function Invoke-AmElevatedPowerShell {
    param([string]$Arguments)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $psi.Arguments = $Arguments
    $psi.Verb = 'runas'
    $psi.UseShellExecute = $true
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.WaitForExit()
    return $p.ExitCode
}

<#
.SYNOPSIS
    Run a piece of PowerShell WITH administrator rights (one UAC prompt), with this core loaded, and print
    its record here when it ends.
.DESCRIPTION
    Only the step that needs the rights runs elevated (an all-users install, update or uninstall); the
    calling script stays unelevated, so whatever it starts afterwards (the app) runs without them. The
    phase script lives in this user's temp folder (administrators can read it, also when UAC was answered
    with another administrator's credentials).
.PARAMETER Body
    The phase's own commands; they may use $PhaseDir.
.PARAMETER Files
    Paths copied into the phase folder first.
.OUTPUTS
    [pscustomobject] Code (the exit code; 1223 when the UAC prompt was declined, 1260 when Windows refuses
    administrator rights to this account by policy) and Result (the text of $PhaseDir\result.txt, '' when
    the phase wrote none).
#>
function Invoke-AmElevatedPhase {
    param([string]$Body, [string[]]$Files = @())
    $phaseDir = Join-Path $env:TEMP ('Agentmaster-elevated-' + [guid]::NewGuid().ToString('N').Substring(0, 12))
    New-Item -ItemType Directory -Force -Path $phaseDir | Out-Null
    foreach ($f in $Files) { Copy-Item -LiteralPath $f -Destination $phaseDir -Recurse -Force }
    $ps1 = Write-AmPhaseScript -PhaseDir $phaseDir -Body $Body
    $code = 0
    try {
        $code = Invoke-AmElevatedPowerShell -Arguments "-NoProfile -ExecutionPolicy Bypass -File `"$ps1`""
    } catch {
        # 1223 (ERROR_CANCELLED) = the UAC prompt was declined; 1260 (ERROR_ACCESS_DISABLED_BY_POLICY) = this
        # PC refuses administrator rights to this account (a standard user under "Automatically deny elevation
        # requests" - seen in a test desktop, 2026-10-21). Without a Win32 code it reads as declined.
        $code = Get-AmWin32ErrorCode -ErrorRecord $_ -Default 1223
        if ($code -eq 1260) { Write-Warn 'Windows does not let this account run programs as administrator (a setting of this PC)' }
        elseif ($code -eq 1223) { Write-Warn 'administrator approval was declined' }
        else { Write-Warn "the step that needs administrator rights could not start: $($_.Exception.Message)" }
    }
    $result = Read-AmPhaseRecord -PhaseDir $phaseDir
    Remove-Item -LiteralPath $phaseDir -Recurse -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ Code = $code; Result = $result }
}

<#
.SYNOPSIS
    Write a phase script: this core's functions and lists, the phase folder, a transcript, the body, and an
    exit code (0, or 1 with the error printed).
.PARAMETER PhaseDir
    The phase folder.
.PARAMETER Body
    The phase's commands.
.OUTPUTS
    [string] the phase.ps1 path.
#>
function Write-AmPhaseScript {
    param([string]$PhaseDir, [string]$Body)
    $log = Join-Path $PhaseDir 'phase.log'
    $q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
    $text = "`$ErrorActionPreference = 'Stop'`r`n" +
            "`$PhaseDir = " + (& $q $PhaseDir) + "`r`n" +
            "Start-Transcript -LiteralPath " + (& $q $log) + " | Out-Null`r`n" +
            "`$script:AmPreservedNames = @(" + (($script:AmPreservedNames | ForEach-Object { & $q $_ }) -join ',') + ")`r`n" +
            "`$script:AmLegacyShippedNames = @(" + (($script:AmLegacyShippedNames | ForEach-Object { & $q $_ }) -join ',') + ")`r`n" +
            "`$script:AmLegacyShippedPatterns = @(" + (($script:AmLegacyShippedPatterns | ForEach-Object { & $q $_ }) -join ',') + ")`r`n" +
            "`$script:AmSetAsidePrefix = " + (& $q $script:AmSetAsidePrefix) + "`r`n" +
            "`$script:AmReleaseFamily = " + (& $q $script:AmReleaseFamily) + "`r`n" +
            "`$script:AmCoreFunctionNames = @(" + (($script:AmCoreFunctionNames | ForEach-Object { & $q $_ }) -join ',') + ")`r`n" +
            (Export-AmFunctions -Names $script:AmCoreFunctionNames) + "`r`n" +
            "try {`r`n" + $Body + "`r`n  Stop-Transcript | Out-Null; exit 0`r`n} catch {`r`n  Write-Host ('Failed: ' + `$_.Exception.Message) -ForegroundColor Red; Stop-Transcript | Out-Null; exit 1`r`n}`r`n"
    $ps1 = Join-Path $PhaseDir 'phase.ps1'
    # A BOM: the phase may carry non-ASCII user paths (a user name with an umlaut), which Windows PowerShell 5.1 would read as ANSI.
    [IO.File]::WriteAllText((Resolve-AmPath $ps1), $text, (New-Object Text.UTF8Encoding($true)))
    return $ps1
}

<#
.SYNOPSIS
    Print a finished phase's transcript (without its banner) and return its result.txt.
.PARAMETER PhaseDir
    The phase folder.
.OUTPUTS
    [string] the result text, '' when none.
#>
function Read-AmPhaseRecord {
    param([string]$PhaseDir)
    $log = Join-Path $PhaseDir 'phase.log'
    if (Test-Path -LiteralPath $log) {
        $inBanner = $false
        foreach ($line in Get-Content -LiteralPath $log) {
            if ($line -match '^\*{10,}') { $inBanner = -not $inBanner; continue }
            if ($inBanner -or -not $line -or $line -match '^Transcript started') { continue }
            Write-Host "    | $line"
        }
    }
    $result = Join-Path $PhaseDir 'result.txt'
    if (Test-Path -LiteralPath $result) { return ([IO.File]::ReadAllText((Resolve-AmPath $result))).Trim() }
    return ''
}

# The core's function names, in dependency-free order - what Invoke-AmPhaseAsUser exports to a phase script.
$script:AmCoreFunctionNames = @(
    'Write-Step', 'Write-Ok', 'Write-Warn', 'Write-Info', 'Resolve-AmPath', 'Initialize-Net', 'Import-AppxModule', 'Get-OSArch',
    'Test-Admin', 'Test-CanElevate', 'Get-AmIdentity', 'Get-FileSha256', 'Test-CertInMachineStore',
    'Test-CertTrustedMachine', 'Approve-SigningCert', 'Get-VCLibs', 'Get-AmPackage', 'Remove-AmPackage',
    'Stop-ProcessesUnder', 'Test-SafeManifestName', 'Read-AmManifest', 'Get-AmShippedNames', 'Remove-AmEntries',
    'Install-AmLayout', 'Restore-AmSetAside', 'Get-AppExe', 'Get-AmLayoutRoot', 'Get-AmInstallDecision', 'Set-AmInstallDecision',
    'Get-AmUserProfileDir', 'Set-AmProfilePointer', 'Test-AmDirWritable', 'Get-AmRegistrationTargets',
    'Send-AmEnvironmentChange', 'Format-AmVersion', 'Get-AmRegistrationName', 'Test-AmShortcutInto', 'Register-AmInstall',
    'Unregister-AmInstall', 'Uninstall-AmCopy',
    'Invoke-AmAsUser', 'Start-AmAppAsUser', 'Export-AmFunctions', 'Invoke-AmPhaseAsUser',
    'Copy-AmProfileData', 'Remove-AmLegacyScriptShortcut',
    'Install-AmUserCopy', 'Install-AmMachineCopy', 'Get-AmDefaultUserInstallDir', 'Get-AmDefaultMachineInstallDir',
    'Find-AmUserInstallDir', 'Find-AmMachineInstallDir', 'Get-AmInstalledVersion', 'Expand-AmZip',
    'Invoke-AmUserInstall', 'Invoke-AmMachineInstall', 'Get-AmWin32ErrorCode', 'Invoke-AmElevatedPowerShell', 'Invoke-AmElevatedPhase', 'Write-AmPhaseScript',
    'Read-AmPhaseRecord')
# <<<<<<<< AGENTMASTER SHARED INSTALL CORE <<<<<<<<

# ---- this script's own helpers ---------------------------------------------------------------------------
<#
.SYNOPSIS
    Wait (up to 20 s) for the app process that started this script to exit, so its files are free.
.NOTES
    Never throws: a timeout proceeds, and the swap's own process stop handles a straggler.
#>
function Wait-ForExit {
    if ($WaitPid -gt 0) {
        Write-Step 'Waiting for Agentmaster to close'
        try { Wait-Process -Id $WaitPid -Timeout 20 -ErrorAction SilentlyContinue } catch { Write-Info "wait ended: $($_.Exception.Message)" }
    }
}

<#
.SYNOPSIS
    True when an Agentmaster GUI process of THIS account runs from $Dir.
.DESCRIPTION
    Only this account's processes count (their owner SID, the way Stop-ProcessesUnder tells them apart):
    another user's instance of an all-users copy is VISIBLE to an unelevated query - its path and its owner
    both (measured in a test desktop on 2026-10-21) - and counting it kept the update from starting this
    user's app again after a cancelled update, while that other user's instance ran.
.PARAMETER Dir
    An install folder.
.PARAMETER ExceptPid
    A process id that does not count (the closing app this script waits for).
.OUTPUTS
    [bool]
.NOTES
    Never throws: a failed query reads as "not running", and a process whose owner cannot be read is not
    this account's (its own processes always can be).
#>
function Test-AmAppRunningUnder {
    param([string]$Dir, [int]$ExceptPid = 0)
    if (-not $Dir) { return $false }
    try {
        $prefix = (Resolve-AmPath $Dir).TrimEnd('\') + '\'
        $mine = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        $running = @(Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe' OR Name='WindowsTerminal.exe'" -ErrorAction Stop |
            Where-Object { $_.ProcessId -ne $ExceptPid -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } |
            Where-Object {
                $owner = $null
                try { $owner = (Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid -ErrorAction Stop).Sid } catch { $owner = $null }
                $owner -eq $mine
            })
        return ($running.Count -gt 0)
    } catch {
        return $false
    }
}

<#
.SYNOPSIS
    Leave the one-shot marker that tells the app this script starts again that the update did not complete
    (Updater.h ConsumeRestartedAfterUpdateMarker): that run then behaves as "Remind me next restart" instead of
    offering the same update again at once (seen in the test desktop on 2026-10-21, right after a cancel).
.PARAMETER Dir
    The profile folder (-ProfileDir); '' = no marker (an app older than -ProfileDir passes none).
.NOTES
    Never throws: without the marker the restarted app only asks once more.
#>
function Write-RestartedMarker {
    param([string]$Dir)
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir)) { return }
    try {
        $tag = if ($Version) { $Version } else { 'unknown' }
        $text = $tag + "`n" + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + "`n"
        [IO.File]::WriteAllText((Join-Path (Resolve-AmPath $Dir) 'am-update-restarted.txt'), $text, (New-Object Text.UTF8Encoding($false)))
    } catch {
        Write-Info "the restart marker was not written: $($_.Exception.Message)"
    }
}

<#
.SYNOPSIS
    Start the INSTALLED (previous) version again after an update that did not complete, was cancelled, or
    kept the installed version - the app closed itself to let the update run, and an update must never leave
    the user without it (the user's rule of 2026-10-21).
.DESCRIPTION
    Runs once per script run, and only in an update mode that has not started an app yet. It first waits for
    the closing app (a start while it still closes would hand the launch to the dying instance and vanish
    with it), then starts the copy in -PortableDir, or the MSIX package, as this account - the account the
    app ran as. Nothing is started when Agentmaster already runs from there again (the user started it
    meanwhile). Right before it starts one, it leaves the marker that keeps that run from offering the same
    update again at once (Write-RestartedMarker).
.NOTES
    Never throws: a failure is reported with how to start Agentmaster by hand.
#>
function Restart-PreviousApp {
    if ($script:AmAppStarted -or -not $script:AmIsUpdateRun) { return }
    $script:AmAppStarted = $true # once per run, whatever happens below
    try {
        Wait-ForExit
        if ($WaitPid -gt 0 -and (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue)) {
            Write-Warn "the previous Agentmaster (pid $WaitPid) has not closed yet: start Agentmaster again once it has"
            return
        }
        if ($Portable) {
            $exe = if ($PortableDir) { Get-AppExe $PortableDir } else { $null }
            if (-not $exe) { Write-Warn "no Agentmaster is left in $PortableDir to start: start it from the Start menu"; return }
            if (Test-AmAppRunningUnder -Dir $PortableDir -ExceptPid $WaitPid) { Write-Info 'Agentmaster is running again already'; return }
            Write-Step 'Starting the installed version again'
            Write-RestartedMarker -Dir $ProfileDir
            Start-Process -FilePath $exe
            return
        }
        Import-AppxModule
        $pkg = Get-AmPackage -FamilyName $Family
        if (-not $pkg) { Write-Warn 'the Agentmaster package is not installed any more: there is nothing to start'; return }
        if ($pkg.InstallLocation -and (Test-AmAppRunningUnder -Dir ([string]$pkg.InstallLocation) -ExceptPid $WaitPid)) { Write-Info 'Agentmaster is running again already'; return }
        Write-Step 'Starting the installed version again'
        Write-RestartedMarker -Dir $ProfileDir
        Start-Process "shell:appsFolder\$Aumid"
    } catch {
        Write-Warn "Agentmaster could not be started again ($($_.Exception.Message)): start it from the Start menu"
    }
}

<#
.SYNOPSIS
    Wait up to $Seconds for a key press in this console, so a failure can be read before the window goes on.
.PARAMETER Seconds
    The longest wait.
.PARAMETER Prompt
    The line shown while waiting.
.NOTES
    Never throws: without a console to read (a redirected input) it returns at once.
#>
function Wait-ForKeyOrTimeout {
    param([int]$Seconds, [string]$Prompt)
    Write-Host $Prompt -ForegroundColor DarkGray
    try {
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            if ([Console]::KeyAvailable) { $null = [Console]::ReadKey($true); return }
            Start-Sleep -Milliseconds 200
        }
    } catch {
        Write-Info "no console to wait on ($($_.Exception.Message))"
    }
}

<#
.SYNOPSIS
    The choices after the all-users copy could not be updated: the reason, the option lines, the accepted
    answers and the default (the pure half of Read-MachineUpdateChoice).
.PARAMETER Code
    The elevated swap's result: 1223 = the approval was declined, 1260 = this PC refuses administrator rights
    to this account (asking again cannot help, so that answer is not offered), anything else = the swap ran
    and failed (its record is printed above; another user still running Agentmaster from the folder is the
    usual cause, so asking again later can work).
.OUTPUTS
    [pscustomobject] Reason, Lines, Answers (answer letter -> admin | user | cancel), Default ('user': it
    needs no administrator rights, keeps the user's data and leaves the all-users copy to the other users -
    the per-user install the user prefers).
#>
function Get-MachineUpdateChoices {
    param([int]$Code)
    $reason = if ($Code -eq 1223) { 'Administrator approval was declined, so the copy installed for all users of this PC was not updated.' }
              elseif ($Code -eq 1260) { 'Windows does not let this account run programs as administrator (a setting of this PC), so the copy installed for all users cannot be updated from here.' }
              else { "The update of the copy installed for all users did not complete (exit code $Code; its record is above)." }
    $what = if ($Version) { "Agentmaster $Version" } else { 'the new version' }
    $lines = @()
    $answers = @{ 'u' = 'user'; 'c' = 'cancel'; 'n' = 'cancel' }
    if ($Code -ne 1260) {
        $lines += '[A] Ask for administrator approval again' + $(if ($Code -ne 1223) { ' (for example once the other users have closed Agentmaster)' } else { '' })
        $answers['a'] = 'admin'
    }
    $lines += "[U] Install $what just for you instead - no administrator rights, now or later; your sessions and settings are kept, and the copy for all users stays for the other users"
    $lines += '[C] Cancel - keep the installed version (it starts again)'
    return [pscustomobject]@{ Reason = $reason; Lines = $lines; Answers = $answers; Default = 'user' }
}

<#
.SYNOPSIS
    Ask, in this console, what to do after the all-users copy could not be updated (Get-MachineUpdateChoices).
.PARAMETER Code
    The elevated swap's result (see Get-MachineUpdateChoices).
.OUTPUTS
    [string] admin, user or cancel; the default when the console cannot be read.
#>
function Read-MachineUpdateChoice {
    param([int]$Code)
    $c = Get-MachineUpdateChoices -Code $Code
    Write-Host ''
    Write-Host "    $($c.Reason)" -ForegroundColor Yellow
    Write-Host ''
    foreach ($line in $c.Lines) { Write-Host "      $line" }
    Write-Host ''
    while ($true) {
        try { $ans = Read-Host '    Choice (Enter = U)' } catch { return $c.Default }
        $a = ("$ans").Trim().ToLowerInvariant()
        if (-not $a) { return $c.Default }
        if ($c.Answers.ContainsKey($a)) { return $c.Answers[$a] }
        Write-Warn "unrecognized answer '$ans'"
    }
}

<#
.SYNOPSIS
    Instead of updating a copy this account cannot write (the all-users install): install the downloaded
    version just for this user - no administrator rights, profile.path -> the profile the app used
    (-ProfileDir) - and start it.
.DESCRIPTION
    The all-users copy stays as it is, for the other users of the PC. The two are told apart by their names
    (Get-AmRegistrationName): "Agentmaster" is the user's own, "Agentmaster (all users)" the shared one.
.PARAMETER ZipPath
    The downloaded release zip.
.NOTES
    Throws when the per-user install fails (the main block then starts the installed version again).
#>
function Invoke-UserInstallInstead {
    param([string]$ZipPath)
    Write-Step 'Installing the new version just for you (no administrator rights)'
    $num = Get-NumericVersion $Version
    if (-not $num) { $num = '0.0.0' }
    $exe = Invoke-AmUserInstall -ZipPath $ZipPath -Version $num -ProfileDir $ProfileDir
    Write-Step 'Starting Agentmaster'
    $script:AmAppStarted = Start-AmAppAsUser -Exe $exe -Identity (Get-AmIdentity)
    Write-Host ''
    Write-Host "Agentmaster $Version is installed just for you at $(Split-Path -Parent $exe)." -ForegroundColor White
    Write-Info 'It updates without administrator rights. In the Start menu it is "Agentmaster"; "Agentmaster (all users)" is the copy every user of this PC shares, left as it was.'
    Start-Sleep -Seconds 3
}

<#
.SYNOPSIS
    The numeric part of a release version ("v0.8.0-prerelease" -> "0.8.0"), as the .am-version stamp and
    Apps & Features use it.
.PARAMETER Text
    The version text the app passed.
.OUTPUTS
    [string] the numeric version, or '' when it does not parse.
#>
function Get-NumericVersion {
    param([string]$Text)
    $num = (($Text -replace '^[vV]', '') -split '-')[0]
    try { return ([version]$num).ToString() } catch { return '' }
}

<#
.SYNOPSIS
    True when a file matches a "sha256:<hex>" (or bare hex) digest; true when there is nothing to verify.
.PARAMETER Path
    The downloaded file.
.PARAMETER Sha256
    The expected digest; empty, or not 64 hex digits, means "nothing to verify" (an old app passes none).
.OUTPUTS
    [bool]
.NOTES
    Throws only when the file cannot be read for hashing.
#>
function Test-Hash {
    param($Path, $Sha256)
    if (-not $Sha256) { return $true }
    $want = ($Sha256 -replace '^sha256:', '').Trim()
    if ($want -notmatch '^[0-9a-fA-F]{64}$') { return $true }
    $have = Get-FileSha256 $Path
    return ($have -ieq $want)
}

<#
.SYNOPSIS
    Download $Url to $Dest unless a verified copy is cached.
.PARAMETER Url
    The file's URL.
.PARAMETER Dest
    The local file. An existing one is reused only when $Sha256 is given and matches: without a digest
    there is no way to tell a complete earlier download from a cut-off one.
.PARAMETER Sha256
    The expected digest ("sha256:<hex>" or bare hex), or '' for none.
.NOTES
    Throws on a download failure or a checksum mismatch.
#>
function Save-File {
    param($Url, $Dest, $Sha256)
    if ((Test-Path $Dest) -and $Sha256 -and (Test-Hash $Dest $Sha256)) {
        Write-Ok ("cached  " + [IO.Path]::GetFileName($Dest))
        return
    }
    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing -Headers @{ 'User-Agent' = 'Agentmaster-Updater' }
    } finally { $ProgressPreference = $old }
    if ($Sha256 -and -not (Test-Hash $Dest $Sha256)) {
        throw "Checksum mismatch for $([IO.Path]::GetFileName($Dest)) - download corrupt; re-run the update."
    }
    Write-Ok ("downloaded  " + [IO.Path]::GetFileName($Dest))
}

<#
.SYNOPSIS
    Remove an install that blocks the package update (0x80073CFB: a registered loose-layout dev install, or
    any same-identity install that cannot be replaced in place) WITHOUT asking - the user already clicked
    "Update now" in the app (DESIGNED DIVERGENCE from Install-Agentmaster.ps1, which confirms).
.NOTES
    Throws when the blocker cannot be found or removed. The data lives outside the package and survives.
#>
function Remove-BlockingInstall {
    $pkg = Get-AmPackage -FamilyName $Family
    if (-not $pkg) {
        throw "A conflicting Agentmaster install is blocking the update but could not be found to remove automatically. Remove it manually and re-run:  Get-AppxPackage Agentmaster | Remove-AppxPackage"
    }
    $kind = if ($pkg.IsDevelopmentMode) { 'a registered (unpackaged) layout' } else { 'a packaged install' }
    Write-Warn "removing a conflicting existing install ($kind, version $($pkg.Version)); your data is kept"
    Remove-AmPackage -Package $pkg
}

<#
.SYNOPSIS
    Add-AppxPackage the bundle, ALWAYS force-applied (this IS an update the user asked for), resolving a
    missing VCLibs dependency or a blocking install once (DESIGNED DIVERGENCE from Install-Agentmaster.ps1).
.PARAMETER BundlePath
    The downloaded .msixbundle.
.NOTES
    Throws when the install fails for another reason.
#>
function Install-Bundle {
    param($BundlePath)
    <#
    .SYNOPSIS
        One force-applied Add-AppxPackage of $BundlePath, with dependency packages when given.
    .PARAMETER deps
        Dependency package paths (VCLibs), or $null for none.
    .NOTES
        Throws whatever Add-AppxPackage throws (the caller sorts the error out).
    #>
    function Add-It($deps) {
        if ($deps) { Add-AppxPackage -Path $BundlePath -DependencyPath $deps -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop }
        else       { Add-AppxPackage -Path $BundlePath -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop }
    }
    try {
        Add-It $null
    } catch {
        $msg = $_.Exception.Message
        if ($msg -match '0x80073CF3' -or $msg -match 'dependency') {
            Write-Warn 'resolving the framework dependency (VCLibs)...'
            Add-It (Get-VCLibs $Dir)
        }
        elseif ($msg -match '0x80073CFB' -or $msg -match 'already installed' -or $msg -match 'cannot replace' -or $msg -match 'unpackaged') {
            Remove-BlockingInstall
            Add-It $null
        }
        else { throw }
    }
}

<#
.SYNOPSIS
    The certificate question of a package update in a console window: approve as administrator (UAC),
    switch to the per-user install (no administrator rights, the same data; offered when -ZipUrl came
    along), or cancel. The default is the approval when this account can elevate with one click, else the
    switch.
.OUTPUTS
    [string] admin, user or cancel.
#>
function Read-UpdateCertChoice {
    $canElevate = Test-CanElevate
    $canSwitch = [bool]$ZipUrl
    Write-Host ''
    Write-Host "    This update is signed with a new certificate, which must be trusted for the whole PC once" -ForegroundColor Yellow
    Write-Host "    (every Agentmaster release has its own) - that needs administrator approval." -ForegroundColor Yellow
    if (-not $canElevate) { Write-Host "    Your account is not an administrator: Windows will ask for an administrator's password." -ForegroundColor Yellow }
    Write-Host ''
    Write-Host '      [A] Approve as administrator (UAC), then update the package'
    if ($canSwitch) { Write-Host '      [U] Switch to the per-user install - no administrator rights now or later; your sessions and settings are kept' }
    # The same promise as the all-users swap's [C] (Get-MachineUpdateChoices): the main block's
    # Restart-PreviousApp starts the package again after a cancel, so the user is never left without the app.
    Write-Host '      [C] Cancel - keep the installed version (it starts again)'
    Write-Host ''
    $def = if ($canElevate -or -not $canSwitch) { 'admin' } else { 'user' }
    $answers = @{ 'a' = 'admin'; 'c' = 'cancel'; 'n' = 'cancel' }
    if ($canSwitch) { $answers['u'] = 'user' }
    while ($true) {
        try { $ans = Read-Host ("    Choice (Enter = " + $(if ($def -eq 'admin') { 'A' } else { 'U' }) + ")") } catch { return $def }
        $a = ("$ans").Trim().ToLowerInvariant()
        if (-not $a) { return $def }
        if ($answers.ContainsKey($a)) { return $answers[$a] }
        Write-Warn "unrecognized answer '$ans'"
    }
}

<#
.SYNOPSIS
    Replace the package with the per-user install: download the release zip, install it for this user
    (no administrator rights; profile.path -> the profile the package used - -ProfileDir when the app passed
    it - so every session stays), remove the package, start the new copy.
.NOTES
    Throws when the download or the install fails (the package then stays installed and usable, and the main
    block starts it again).
#>
function Invoke-SwitchToUserInstall {
    Write-Step 'Switching to the per-user install (no administrator rights)'
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $zip = Join-Path $Dir ([IO.Path]::GetFileName(([Uri]$ZipUrl).AbsolutePath))
    Save-File $ZipUrl $zip $ZipSha256
    Wait-ForExit
    $num = Get-NumericVersion $Version
    if (-not $num) { $num = '0.0.0' }
    $exe = Invoke-AmUserInstall -ZipPath $zip -Version $num -RemovePackage -ProfileDir $ProfileDir
    Write-Step 'Starting Agentmaster'
    $script:AmAppStarted = Start-AmAppAsUser -Exe $exe -Identity (Get-AmIdentity)
    Write-Host ''
    Write-Host "Switched: Agentmaster $Version is installed just for you at $(Split-Path -Parent $exe)." -ForegroundColor White
    Write-Info 'Its updates need no administrator rights.'
    Start-Sleep -Seconds 3
}

# ---- the modes -------------------------------------------------------------------------------------------
<#
.SYNOPSIS
    The package update (the default mode).
.NOTES
    Throws on any failure (the main block shows it and starts the installed package again). A cancel returns
    without starting anything: the main block then starts the installed package again too.
#>
function Invoke-Install {
    Write-Host ''
    Write-Host "Agentmaster updater - installing $Version" -ForegroundColor White
    Write-Host ''
    if (-not $BundleUrl -or -not $CerUrl) { throw 'Internal error: the update was launched without a bundle/certificate URL.' }
    Import-AppxModule
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $bundle = Join-Path $Dir ([IO.Path]::GetFileName(([Uri]$BundleUrl).AbsolutePath))
    $cer    = Join-Path $Dir 'Agentmaster.cer'

    Write-Step 'Downloading the update'
    Save-File $CerUrl $cer ''
    # Every release has its own certificate: an update needs an administrator exactly when THIS one is not
    # trusted yet. Ask before the big download, so a switch or a cancel wastes nothing.
    if (-not (Test-Admin) -and -not (Test-CertTrustedMachine $cer)) {
        switch (Read-UpdateCertChoice) {
            'user'   { Invoke-SwitchToUserInstall; return }
            'cancel' { Write-Warn 'Cancelled - the installed version stays, and starts again.'; Start-Sleep -Seconds 2; return }
        }
    }
    # The certificate is trusted BEFORE the big bundle download: a declined or refused approval then costs
    # nothing, and the switch to the per-user install downloads only the zip.
    try {
        Approve-SigningCert -CerPath $cer
    } catch {
        if (-not $ZipUrl) { throw }
        Write-Warn $_.Exception.Message
        if ((Read-UpdateCertChoice) -eq 'user') { Invoke-SwitchToUserInstall; return }
        throw
    }
    Save-File $BundleUrl $bundle $BundleSha256

    Wait-ForExit

    Write-Step "Installing $Version"
    Install-Bundle -BundlePath $bundle
    Write-Ok 'installed'

    $now = Get-AmPackage -FamilyName $Family
    if (-not $now) { throw 'Install reported success but the package is not registered. See the error above.' }

    Write-Step 'Relaunching Agentmaster'
    Start-Process "shell:appsFolder\$Aumid"
    $script:AmAppStarted = $true
    $shown = if ($Version) { $Version } else { "$($now.Version)" }
    Write-Host ''
    Write-Host "Updated to $shown." -ForegroundColor White
    Start-Sleep -Seconds 2
}

<#
.SYNOPSIS
    The in-place update of a portable / per-user / all-users copy (-Portable -PortableDir -ZipUrl).
.DESCRIPTION
    Replaces only what Agentmaster ships (Install-AmLayout: the old manifest, else the legacy list, plus the
    new layout's names) and keeps settings\, profile\, profile.path, install.path and a user's own files.
    A folder this account cannot write (the all-users install, or an administrator's copy in Program Files)
    gets only its swap elevated (one UAC prompt); this script, and so the relaunched app, stays unelevated.
    When that swap does not happen - the approval declined, refused by this PC's policy, or the swap failed
    (another user still runs Agentmaster from the folder) - the user is asked (Read-MachineUpdateChoice):
    ask again, install the new version just for themselves (Invoke-UserInstallInstead), or cancel. Before
    2026-10-21 it ended there with the app closed: a demoted user had no way left to update.
.NOTES
    Throws on a failed download or swap (the swap first puts the previous version back; the main block then
    starts it again). A cancel returns without starting anything, and the main block starts the installed
    version again.
#>
function Invoke-PortableUpdate {
    Write-Host ''
    Write-Host "Agentmaster updater - installing $Version (in place)" -ForegroundColor White
    Write-Host ''
    if (-not $ZipUrl -or -not $PortableDir) { throw 'Internal error: the portable update was launched without a zip URL / target folder.' }
    if (-not (Get-AppExe $PortableDir)) {
        throw "The portable folder no longer holds the app exe (Agentmaster.exe): $PortableDir"
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $zip = Join-Path $Dir ([IO.Path]::GetFileName(([Uri]$ZipUrl).AbsolutePath))

    Write-Step 'Downloading the update'
    Save-File $ZipUrl $zip $ZipSha256

    Wait-ForExit
    $num = Get-NumericVersion $Version
    $q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
    # The swap itself, written once as text so it can run here OR in an elevated phase: close what runs from
    # the folder, lay the new layout in, stamp the version (the zip ships its own stamp; this belt covers a
    # suffixed tag), and re-stamp an all-users install's machine registration.
    # Its own work folder (removed whole afterwards): a flat zip's layout root IS the extraction folder, so
    # cleaning up "the root's parent" could hit this script's own folder.
    $work = Join-Path $Dir ('swap-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $swap = "Stop-ProcessesUnder -DirPath " + (& $q $PortableDir) + "`r`n" +
            "New-Item -ItemType Directory -Force -Path " + (& $q $work) + " | Out-Null`r`n" +
            "`$root = Expand-AmZip -ZipPath " + (& $q $zip) + " -WorkDir " + (& $q $work) + "`r`n" +
            "Install-AmLayout -SourceDir `$root -DestDir " + (& $q $PortableDir) + "`r`n" +
            "Remove-Item -LiteralPath " + (& $q $work) + " -Recurse -Force -ErrorAction SilentlyContinue`r`n" +
            $(if ($num) { "[IO.File]::WriteAllText((Join-Path " + (& $q $PortableDir) + " '.am-version'), " + (& $q $num) + ")`r`n" } else { '' }) +
            "if ((Get-AmInstallDecision -Dir " + (& $q $PortableDir) + ") -eq 'machine') { foreach (`$n in (Register-AmInstall -Scope Machine -Dir " + (& $q $PortableDir) + " -Version " + (& $q $num) + ")) { Write-Warn `$n } }`r`n" +
            "Write-Ok 'installed'"
    if (Test-AmDirWritable $PortableDir) {
        Write-Step "Installing $Version into $PortableDir"
        Invoke-Expression $swap
    } else {
        Write-Step "Installing $Version into $PortableDir - it is installed for all users: Windows asks for administrator approval"
        while ($true) {
            $r = Invoke-AmElevatedPhase -Body $swap
            if ($r.Code -eq 0) { break }
            # A switch inside a while loop would catch the `continue`: plain ifs.
            $choice = Read-MachineUpdateChoice -Code $r.Code
            if ($choice -eq 'admin') { Write-Step 'Asking for administrator approval again'; continue }
            # A swap that failed after its expansion leaves its work folder behind (it is removed only at its end).
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
            if ($choice -eq 'user') { Invoke-UserInstallInstead -ZipPath $zip; return }
            Write-Warn 'Cancelled - the installed version stays, and starts again.'
            Start-Sleep -Seconds 2
            return
        }
    }

    Write-Step 'Relaunching Agentmaster'
    $exe = Get-AppExe $PortableDir
    if ($exe) { Start-Process $exe; $script:AmAppStarted = $true } else { Write-Warn 'app exe not found after the swap - launch it manually' }
    Write-Host ''
    Write-Host "Updated to $Version." -ForegroundColor White
    Start-Sleep -Seconds 2
}

<#
.SYNOPSIS
    Remove the MSIX package (-Uninstall; per-user, no administrator rights).
.NOTES
    Throws when Remove-AppxPackage fails.
#>
function Invoke-Uninstall {
    Write-Host ''
    Write-Host 'Agentmaster uninstaller' -ForegroundColor White
    Write-Host ''
    Import-AppxModule
    Wait-ForExit
    $pkg = Get-AmPackage -FamilyName $Family
    if (-not $pkg) {
        Write-Warn "Agentmaster ($Family) is not installed - nothing to remove."
        Start-Sleep -Seconds 2
        return
    }
    Write-Step "Removing $($pkg.PackageFullName)"
    Remove-AmPackage -Package $pkg
    Write-Ok 'uninstalled'
    Write-Info 'Your profile data (e.g. %USERPROFILE%\.agentmaster) was left untouched.'
    Start-Sleep -Seconds 2
}

<#
.SYNOPSIS
    Remove a per-user install (-UninstallPortable -PortableDir): only its own registration and shipped files.
.NOTES
    Throws when -PortableDir is missing.
#>
function Invoke-PortableUninstall {
    Write-Host ''
    Write-Host 'Agentmaster uninstaller (installed just for you)' -ForegroundColor White
    Write-Host ''
    if (-not $PortableDir) { throw 'Internal error: the portable uninstall was launched without a target folder.' }
    Wait-ForExit
    $null = Uninstall-AmCopy -Scope User -Dir $PortableDir
    Write-Host ''
    Write-Host 'Agentmaster was uninstalled.' -ForegroundColor White
    Write-Info 'Your profile data (sessions, settings, window layouts) was left untouched.'
    Start-Sleep -Seconds 3
}

<#
.SYNOPSIS
    Remove the all-users install (-UninstallMachine -PortableDir; the app runs this ELEVATED).
.NOTES
    Throws without administrator rights, or while other users still run Agentmaster from the folder.
#>
function Invoke-MachineUninstall {
    Write-Host ''
    Write-Host 'Agentmaster uninstaller (installed for all users)' -ForegroundColor White
    Write-Host ''
    if (-not $PortableDir) { throw 'Internal error: the uninstall was launched without a target folder.' }
    if (-not (Test-Admin)) { throw 'Removing the install for all users needs administrator rights.' }
    Wait-ForExit
    $null = Uninstall-AmCopy -Scope Machine -Dir $PortableDir
    Write-Host ''
    Write-Host 'Agentmaster was uninstalled for all users.' -ForegroundColor White
    Write-Info "Every user's own profile data was left untouched."
    Start-Sleep -Seconds 3
}

<#
.SYNOPSIS
    Install the running unzip (-SourceDir) for all users (-InstallMachine; the app runs this ELEVATED and
    waits for the exit code).
.NOTES
    Throws without administrator rights, for a layout without an app exe or manifest, or on a failed copy.
#>
function Invoke-MachineInstall {
    Write-Host ''
    Write-Host 'Agentmaster installer - for all users of this PC' -ForegroundColor White
    Write-Host ''
    if (-not (Test-Admin)) { throw 'Installing for all users needs administrator rights.' }
    if (-not $SourceDir -or -not (Get-AppExe $SourceDir)) { throw "Internal error: no Agentmaster layout at '$SourceDir'." }
    $dest = $DestDir
    if (-not $dest) { $dest = Find-AmMachineInstallDir }
    if (-not $dest) { $dest = Get-AmDefaultMachineInstallDir }
    $ver = Get-AmInstalledVersion -Dir $SourceDir
    $verText = if ($ver) { "$ver" } else { '0.0.0.0' }
    Install-AmMachineCopy -LayoutDir $SourceDir -DestDir $dest -Version $verText
    Write-Host ''
    Write-Host "Agentmaster $verText is installed for all users at $dest." -ForegroundColor White
    Start-Sleep -Seconds 2
}

<#
.SYNOPSIS
    Start -Exe as the user signed in to Windows, without administrator rights (-RunAsMainUser). Hidden:
    the result is the exit code only (see the help's mode list).
#>
function Invoke-RunAsMainUser {
    if (-not $Exe) { exit 4 }
    $arguments = if ($ExeArgs) { [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($ExeArgs)) } else { '' }
    $id = Get-AmIdentity
    if (-not $id.MainUserSid) { exit 3 }
    if (-not ($id.Elevated -or $id.IsSystem)) { exit 2 }
    if (Start-AmAppAsUser -Exe $Exe -Identity $id -Arguments $arguments) { exit 0 }
    exit 4
}

# ============================ main ============================
# Tests dot-source this file to reach its functions without doing anything.
if ($env:AGENTMASTER_INSTALLER_LIBRARY -eq '1') { return }
try {
    Initialize-Net
    if ($RunAsMainUser) { Invoke-RunAsMainUser }
    elseif ($Uninstall) { Invoke-Uninstall }
    elseif ($UninstallPortable) { Invoke-PortableUninstall }
    elseif ($UninstallMachine) { Invoke-MachineUninstall }
    elseif ($InstallMachine) { Invoke-MachineInstall }
    elseif ($Portable) { Invoke-PortableUpdate }
    else { Invoke-Install }
    # An update that ended without starting the app (cancelled, or the installed version kept) starts the
    # installed one again: the app closed itself for the update. A no-op for every other mode.
    Restart-PreviousApp
    exit 0
}
catch {
    Write-Host ''
    Write-Host "Failed: $($_.Exception.Message)" -ForegroundColor Red
    # A hidden run (-RunAsMainUser) has nobody to press a key: it must end, or the app waiting for it hangs.
    if ($RunAsMainUser) { exit 4 }
    Write-Host 'You can download the latest release manually from:' -ForegroundColor DarkGray
    Write-Host '    https://github.com/Nucs/Agentmaster/releases' -ForegroundColor DarkGray
    Write-Host ''
    if ($script:AmIsUpdateRun -and -not $script:AmAppStarted) {
        # The app closed itself for this update: start the installed version again rather than leave the user
        # without it - after a pause in which this window, and why the update failed, can be read.
        Wait-ForKeyOrTimeout -Seconds 30 -Prompt 'Agentmaster starts again in 30 seconds - press any key to start it now...'
        Restart-PreviousApp
        Start-Sleep -Seconds 3
        exit 1
    }
    Write-Host 'Press Enter to close...' -ForegroundColor DarkGray
    try { Read-Host | Out-Null } catch { Write-Host '' }
    exit 1
}
