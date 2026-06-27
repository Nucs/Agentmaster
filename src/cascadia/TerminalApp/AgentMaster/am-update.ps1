# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Agentmaster in-app updater payload (install / upgrade / uninstall).

.DESCRIPTION
    This script is NOT meant to be run by hand. It is embedded as a binary resource (AM_UPDATE_PS1,
    RT_RCDATA) inside WindowsTerminal.exe, read at runtime by Updater.h, materialized into the active
    profile dir, and invoked by the generated am-update.cmd / am-uninstall.cmd launchers.

    It is a trimmed sibling of tools\Install-Agentmaster.ps1 and shares that script's install CORE
    and failure recovery -- keep the two in sync:
      * Downloads the signed .msixbundle + Agentmaster.cer (cached; SHA-256 verified when a digest
        is supplied) into %TEMP%\Agentmaster-update.
      * Trusts the self-signed certificate (LocalMachine\TrustedPeople), elevating ONLY when it is
        not already trusted.
      * Add-AppxPackages it, auto-resolving the VCLibs framework dependency if missing (0x80073CF3)
        AND removing a conflicting existing install that blocks deployment (0x80073CFB -- e.g. a
        registered "loose layout" unpackaged dev install a packaged build cannot replace in place),
        then retrying.
      * Relaunches the app.

    The release was already resolved by the app, so the bundle/cer URLs are passed in rather than
    discovered from the GitHub API (unlike Install-Agentmaster.ps1). The "extra modes" of the
    standalone installer (portable, -Version/-Prerelease resolution) are intentionally omitted.

    -Uninstall removes the installed package (per-user, no admin). Your profile data (e.g.
    %USERPROFILE%\.agentmaster) lives OUTSIDE the package and is never touched.

.NOTES
    Agentmaster -- a fork of Windows Terminal. The released MSIX is self-signed, so trusting its
    certificate requires administrator rights once.
#>
param(
    [string]$BundleUrl,
    [string]$CerUrl,
    [string]$Version      = '',
    [string]$BundleSha256 = '',
    [string]$Family       = 'Agentmaster_56k4f06dsfp9r',
    [int]   $WaitPid      = 0,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$Aumid = "$Family!App"
$Dir   = Join-Path $env:TEMP 'Agentmaster-update'

# ---- tiny console UI ------------------------------------------------------------------------
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "    $m" -ForegroundColor Green }
function Warn($m) { Write-Host "    $m" -ForegroundColor Yellow }
function Info($m) { Write-Host "    $m" -ForegroundColor DarkGray }

# ---- environment helpers --------------------------------------------------------------------
function Initialize-Net {
    # GitHub requires TLS 1.2+; Windows PowerShell 5.1 may default to older protocols.
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
}
function Import-AppxModule {
    # PowerShell 7 reaches the Appx cmdlets through the Windows PowerShell compatibility session.
    if ($PSVersionTable.PSVersion.Major -ge 7) { Import-Module Appx -UseWindowsPowerShell -WarningAction SilentlyContinue -ErrorAction SilentlyContinue }
}
function Get-OSArch {
    switch ($env:PROCESSOR_ARCHITECTURE) { 'ARM64' { 'arm64' } 'AMD64' { 'x64' } 'x86' { 'x86' } default { 'x64' } }
}
function Test-Admin {
    (New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Get-InstalledPackage {
    param($FamilyName)
    Get-AppxPackage | Where-Object { $_.PackageFamilyName -eq $FamilyName } | Select-Object -First 1
}
function Wait-ForExit {
    if ($WaitPid -gt 0) {
        Step 'Waiting for Agentmaster to close'
        try { Wait-Process -Id $WaitPid -Timeout 20 -ErrorAction SilentlyContinue } catch {}
    }
}
# Close any still-running instances of $pkg so its registration releases cleanly. Filtered strictly
# by the package's own InstallLocation, so the Store Windows Terminal and any other-identity install
# (a different path) are never touched.
function Stop-PackageProcesses {
    param($Pkg)
    if (-not $Pkg.InstallLocation) { return }
    try {
        Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Pkg.InstallLocation, [StringComparison]::OrdinalIgnoreCase) } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    } catch {}
}

# ---- downloading ----------------------------------------------------------------------------
function Test-Hash {
    param($Path, $Sha256)
    if (-not $Sha256) { return $true }                          # nothing to verify against
    $want = ($Sha256 -replace '^sha256:', '').Trim()
    if ($want -notmatch '^[0-9a-fA-F]{64}$') { return $true }   # not a usable sha256
    $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    return ($have -ieq $want)
}
function Save-File {
    param($Url, $Dest, $Sha256)
    if ((Test-Path $Dest) -and $Sha256 -and (Test-Hash $Dest $Sha256)) {
        Ok ("cached  " + [IO.Path]::GetFileName($Dest))
        return
    }
    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing -Headers @{ 'User-Agent' = 'Agentmaster-Updater' }
    } finally { $ProgressPreference = $old }
    if ($Sha256 -and -not (Test-Hash $Dest $Sha256)) {
        throw "Checksum mismatch for $([IO.Path]::GetFileName($Dest)) - download corrupt; re-run the update."
    }
    Ok ("downloaded  " + [IO.Path]::GetFileName($Dest))
}

# ---- certificate trust (the only step that may need admin) ----------------------------------
function Approve-SigningCert {
    param($CerPath)
    $full  = (Resolve-Path -LiteralPath $CerPath).Path
    $cert  = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $full
    $thumb = $cert.Thumbprint
    if (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb") { Ok 'signing cert already trusted'; return }
    Step 'Trusting the signing certificate (one-time; may prompt for administrator)'
    if (Test-Admin) {
        Import-Certificate -FilePath $full -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
    } else {
        $p = $full.Replace("'", "''")
        $inner = "try { Import-Certificate -FilePath '$p' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' -ErrorAction Stop | Out-Null; exit 0 } catch { exit 7 }"
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))
        $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -PassThru -Wait -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$enc
        if ($proc.ExitCode -ne 0) { throw 'Administrator elevation was cancelled or failed; the signing certificate must be trusted to install the update.' }
    }
    Ok 'trusted'
}

# ---- framework dependency fallback (offline / Store-less machines) --------------------------
function Get-VCLibs {
    $arch = Get-OSArch
    $dest = Join-Path $Dir "Microsoft.VCLibs.$arch.14.00.Desktop.appx"
    if (-not (Test-Path $dest)) {
        Info "fetching dependency Microsoft.VCLibs ($arch)"
        $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        try { Invoke-WebRequest -Uri "https://aka.ms/Microsoft.VCLibs.$arch.14.00.Desktop.appx" -OutFile $dest -UseBasicParsing } finally { $ProgressPreference = $old }
    }
    return $dest
}

# A conflicting existing registration (an unpackaged / "registered loose layout" dev install, or any
# install of the same identity that can't be replaced in place) makes Add-AppxPackage fail with
# 0x80073CFB. Remove it, then let the caller retry. Per-user removal needs no admin, and Agentmaster's
# data lives OUTSIDE the package (e.g. %USERPROFILE%\.agentmaster), so it survives.
function Remove-BlockingInstall {
    $pkg = Get-InstalledPackage $Family
    if (-not $pkg) {
        throw "A conflicting Agentmaster install is blocking the update but could not be found to remove automatically. Remove it manually and re-run:  Get-AppxPackage Agentmaster | Remove-AppxPackage"
    }
    $kind = if ($pkg.IsDevelopmentMode) { 'a registered (unpackaged) layout' } else { 'a packaged install' }
    Warn "removing a conflicting existing install ($kind, version $($pkg.Version)); your data is kept"
    Stop-PackageProcesses $pkg
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
    Ok ("removed " + $pkg.PackageFullName)
}

function Install-Bundle {
    param($BundlePath)
    function Add-It($deps) {
        if ($deps) { Add-AppxPackage -Path $BundlePath -DependencyPath $deps -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop }
        else       { Add-AppxPackage -Path $BundlePath -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop }
    }
    try {
        Add-It $null
    } catch {
        $msg = $_.Exception.Message
        # (a) Missing framework dependency (VCLibs) -> fetch it and retry.
        if ($msg -match '0x80073CF3' -or $msg -match 'dependency') {
            Warn 'resolving the framework dependency (VCLibs)...'
            Add-It (Get-VCLibs)
        }
        # (b) A conflicting existing install blocks deployment -> remove it and retry.
        elseif ($msg -match '0x80073CFB' -or $msg -match 'already installed' -or $msg -match 'cannot replace' -or $msg -match 'unpackaged') {
            Remove-BlockingInstall
            Add-It $null
        }
        else { throw }
    }
}

# ---- the two operations ---------------------------------------------------------------------
function Invoke-Install {
    Write-Host ''
    Write-Host "Agentmaster updater - installing $Version" -ForegroundColor White
    Write-Host ''
    if (-not $BundleUrl -or -not $CerUrl) { throw 'Internal error: the update was launched without a bundle/certificate URL.' }
    Import-AppxModule
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $bundle = Join-Path $Dir ([IO.Path]::GetFileName(([Uri]$BundleUrl).AbsolutePath))
    $cer    = Join-Path $Dir 'Agentmaster.cer'

    Step 'Downloading the update'
    Save-File $CerUrl    $cer    ''
    Save-File $BundleUrl $bundle $BundleSha256

    Approve-SigningCert -CerPath $cer

    Wait-ForExit

    Step "Installing $Version"
    Install-Bundle -BundlePath $bundle
    Ok 'installed'

    $now = Get-InstalledPackage $Family
    if (-not $now) { throw 'Install reported success but the package is not registered. See the error above.' }

    Step 'Relaunching Agentmaster'
    Start-Process "shell:appsFolder\$Aumid"
    $shown = if ($Version) { $Version } else { "$($now.Version)" }
    Write-Host ''
    Write-Host "Updated to $shown." -ForegroundColor White
    Start-Sleep -Seconds 2
}

function Invoke-Uninstall {
    Write-Host ''
    Write-Host "Agentmaster uninstaller" -ForegroundColor White
    Write-Host ''
    Import-AppxModule
    Wait-ForExit
    $pkg = Get-InstalledPackage $Family
    if (-not $pkg) {
        Warn "Agentmaster ($Family) is not installed - nothing to remove."
        Start-Sleep -Seconds 2
        return
    }
    Step "Removing $($pkg.PackageFullName)"
    Stop-PackageProcesses $pkg
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
    Ok 'uninstalled'
    Info 'Your profile data (e.g. %USERPROFILE%\.agentmaster) was left untouched.'
    Start-Sleep -Seconds 2
}

# ============================ main ============================
try {
    Initialize-Net
    if ($Uninstall) { Invoke-Uninstall } else { Invoke-Install }
}
catch {
    Write-Host ''
    Write-Host "Failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'You can download the latest release manually from:' -ForegroundColor DarkGray
    Write-Host '    https://github.com/Nucs/Agentmaster/releases' -ForegroundColor DarkGray
    Write-Host ''
    Write-Host 'Press Enter to close...' -ForegroundColor DarkGray
    try { Read-Host | Out-Null } catch {}
}
