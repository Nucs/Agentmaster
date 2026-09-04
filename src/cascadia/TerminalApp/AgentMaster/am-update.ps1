# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Agentmaster in-app updater payload (install / upgrade / uninstall).

.DESCRIPTION
    This script is NOT meant to be run by hand. It is embedded as a binary resource (AM_UPDATE_PS1,
    RT_RCDATA) inside the app exe (Agentmaster.exe), read at runtime by Updater.h, materialized into the active
    profile dir, and invoked by the generated am-update.cmd / am-uninstall.cmd launchers.

    It is a trimmed sibling of tools\Install-Agentmaster.ps1 and shares that script's install CORE
    and failure recovery -- KEEP THE TWO IN SYNC. The shared functions (console UI, env helpers,
    Get-FileSha256, the certificate-trust trio, Get-VCLibs) are copied VERBATIM from the installer;
    only Install-Bundle and Remove-BlockingInstall deliberately diverge (marked inline): this script
    runs AFTER the user clicked "Update now" in-app, so it never asks questions -- it force-applies
    and auto-removes a blocking install, where the standalone installer confirms interactively and
    offers the non-admin install-mode menu (which has no meaning here: a packaged install has no
    portable fallback to switch to mid-update).
      * Downloads the signed .msixbundle + Agentmaster.cer (cached; SHA-256 verified when a digest
        is supplied) into %TEMP%\Agentmaster-update.
      * Trusts the self-signed certificate (LocalMachine\TrustedPeople; an already-trusted cert --
        TrustedPeople or Root -- skips straight through), elevating ONLY when it is not trusted yet.
      * Add-AppxPackages it, auto-resolving the VCLibs framework dependency if missing (0x80073CF3)
        AND removing a conflicting existing install that blocks deployment (0x80073CFB -- e.g. a
        registered "loose layout" unpackaged dev install a packaged build cannot replace in place),
        then retrying.
      * Relaunches the app.

    The release was already resolved by the app, so the bundle/cer URLs are passed in rather than
    discovered from the GitHub API (unlike Install-Agentmaster.ps1). The -Version/-Prerelease
    release resolution of the standalone installer is intentionally omitted.

    -Portable performs the PORTABLE in-place update instead (a portable copy = unpackaged + the
    .portable marker; Updater.h routes it here): download the release's arch-matched portable zip
    (-ZipUrl, SHA-256-verified when -ZipSha256 is supplied), stop any instance still running under
    -PortableDir, swap the folder's binaries while PRESERVING the exe-side user state (settings\ +
    profile\ + profile.path -- Install-Agentmaster.ps1's exact preserve list), stamp .am-version
    with the new version (the updater's next check reads it), and relaunch the app exe
    (Agentmaster.exe; a pre-rename folder's WindowsTerminal.exe is recognized too).
    No cert, no admin, no package registration.

    -Uninstall removes the installed package (per-user, no admin). Your profile data (e.g.
    %USERPROFILE%\.agentmaster) lives OUTSIDE the package and is never touched.

    -UninstallPortable (with -PortableDir) removes an INSTALLED PORTABLE copy -- the per-user,
    MSI-like install PortableInstall.h makes on a portable's first launch (`Agentmaster.exe
    --uninstall-portable` is its Apps & Features UninstallString; the cog's Uninstall button
    routes here too): the desktop + Start-menu shortcuts (only those pointing INTO the folder),
    the "Open in Agentmaster" right-click verbs (HKCU\Software\Classes), the Apps & Features
    entry (only when its InstallLocation IS the folder), the folder's user-PATH entry, and the
    binaries. The exe-side user state (settings\ + profile\ + profile.path) is KEPT in place and
    the profile folder is never touched -- reinstalling later finds everything again.

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
    [switch]$Uninstall,
    [switch]$UninstallPortable,
    [switch]$Portable,
    [string]$PortableDir  = '',
    [string]$ZipUrl       = '',
    [string]$ZipSha256    = ''
)

$ErrorActionPreference = 'Stop'
$Aumid = "$Family!App"
$Dir   = Join-Path $env:TEMP 'Agentmaster-update'

# ---- tiny console UI (verbatim from Install-Agentmaster.ps1) --------------------------------
function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }
function Write-Info { param($m) Write-Host "    $m" -ForegroundColor DarkGray }

# ---- environment helpers (verbatim from Install-Agentmaster.ps1) ----------------------------
function Initialize-Net {
    # GitHub requires TLS 1.2+; Windows PowerShell 5.1 may default to older protocols.
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch { }
}

function Import-AppxModule {
    # PowerShell 7 reaches the Appx cmdlets through the Windows PowerShell compatibility session.
    if ($PSVersionTable.PSVersion.Major -ge 7) {
        Import-Module Appx -UseWindowsPowerShell -WarningAction SilentlyContinue -ErrorAction SilentlyContinue
    }
}

function Get-OSArch {
    switch ($env:PROCESSOR_ARCHITECTURE) {
        'ARM64' { 'arm64' }
        'AMD64' { 'x64' }
        'x86'   { 'x86' }
        default { 'x64' }
    }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-InstalledPackage {
    param($FamilyName)
    Get-AppxPackage | Where-Object { $_.PackageFamilyName -eq $FamilyName } | Select-Object -First 1
}

function Wait-ForExit {
    if ($WaitPid -gt 0) {
        Write-Step 'Waiting for Agentmaster to close'
        try { Wait-Process -Id $WaitPid -Timeout 20 -ErrorAction SilentlyContinue } catch {}
    }
}

# Close any still-running instances of $pkg so its registration releases cleanly. Filtered strictly
# by the package's own InstallLocation, so the Store Windows Terminal and any other-identity install
# (a different path) are never touched.
function Stop-PackageProcesses {
    param($Pkg)
    if (-not $Pkg.InstallLocation) { return }
    Stop-ProcessesUnder $Pkg.InstallLocation
}

# The path-filtered process stop both flavors share (verbatim from Install-Agentmaster.ps1's
# Stop-RunningUnder -- KEEP IN SYNC): kill ONLY the GUI process under $DirPath, so the Store
# Windows Terminal and any other install (a different path) are never touched -- and NEVER its
# OpenConsole.exe ConPTY hosts. A host hit by Stop-Process dies without its graceful shutdown (the
# CTRL_CLOSE_EVENT broadcast to the tab's pwsh + claude.exe + MCP children), which leaves every
# session of that tab running forever with a DEAD console (2026-09-04: 35 pwsh+claude pairs and
# ~140 node/cmd children from one such kill, alive six days). Killing the GUI alone closes its pipe
# handles; each host then shuts its clients down itself and exits -- we WAIT for that so the
# binaries are free before the swap / removal (a host locks OpenConsole.exe in $DirPath).
function Stop-ProcessesUnder {
    param($DirPath)
    if (-not $DirPath) { return }
    try {
        $gui = Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe' OR Name='WindowsTerminal.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($DirPath, [StringComparison]::OrdinalIgnoreCase) }
        foreach ($p in $gui) {
            Write-Warn "closing running instance (pid $($p.ProcessId))"
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        # Wait for the ConPTY hosts under $DirPath to drain (each ends its clients -- up to ~5 s per
        # stubborn client -- then exits). 30 s is generous; whatever is still there after that is
        # wedged, and only then is it killed as the last resort (its tab's clients WILL be orphaned).
        $deadline = (Get-Date).AddSeconds(30)
        do {
            Start-Sleep -Milliseconds 500
            $hosts = Get-CimInstance Win32_Process -Filter "Name='OpenConsole.exe'" -ErrorAction SilentlyContinue |
                Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($DirPath, [StringComparison]::OrdinalIgnoreCase) }
        } while ($hosts -and (Get-Date) -lt $deadline)
        foreach ($p in $hosts) {
            Write-Warn "ConPTY host pid $($p.ProcessId) did not exit in 30 s; killing it (its tab's session processes may be orphaned)"
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        if ($gui -or $hosts) { Start-Sleep -Milliseconds 700 }
    } catch {}
}

# ---- downloading (Get-FileSha256 verbatim from Install-Agentmaster.ps1) ---------------------
function Get-FileSha256 {
    param($Path)
    # Raw .NET, NOT Get-FileHash: that cmdlet lives in a MODULE (Microsoft.PowerShell.Utility's
    # script surface) whose auto-load can fail in Windows PowerShell 5.1 under a PS7-polluted
    # PSModulePath (observed live: "The term 'Get-FileHash' is not recognized" killed the whole
    # install) - the same failure class as the Cert:\ drive. .NET needs no module in any host.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs  = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Path).Path)
    try     { return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '') }
    finally { $fs.Dispose(); $sha.Dispose() }
}

function Test-Hash {
    param($Path, $Sha256)
    if (-not $Sha256) { return $true }                          # nothing to verify against
    $want = ($Sha256 -replace '^sha256:', '').Trim()
    if ($want -notmatch '^[0-9a-fA-F]{64}$') { return $true }   # not a usable sha256
    $have = Get-FileSha256 $Path
    return ($have -ieq $want)
}

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

# ---- certificate trust (verbatim from Install-Agentmaster.ps1 - the only step needing admin) --
# AppX deployment validates the package signature in SYSTEM context, so the cert must be trusted
# MACHINE-wide (LocalMachine TrustedPeople or Root) - a CurrentUser store never satisfies it.
# All store access is raw .NET X509Store, NOT the Cert:\ drive / Import-Certificate: the drive
# needs Microsoft.PowerShell.Security, whose load can FAIL in Windows PowerShell 5.1 when a
# PS7-polluted PSModulePath shadows it (observed live: the 7.0.0.0 module resolves first and
# dies on duplicate TypeData, leaving no Cert:\ drive - a trusted cert then silently reads as
# untrusted and the post-import verification false-fails). .NET needs no module in any host.
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

function Test-CertTrustedMachine {
    param($CerPath)
    $full = (Resolve-Path -LiteralPath $CerPath).Path
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $full
    foreach ($store in 'TrustedPeople', 'Root') {
        if (Test-CertInMachineStore $cert.Thumbprint $store) { return $true }
    }
    return $false
}

function Approve-SigningCert {
    param($CerPath)
    $full = (Resolve-Path -LiteralPath $CerPath).Path
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $full
    $thumb = $cert.Thumbprint
    if (Test-CertTrustedMachine $full) {
        Write-Ok "signing cert already trusted ($thumb)"
        return
    }
    Write-Step "Trusting the signing certificate (one-time, requires administrator)"
    if (Test-Admin) {
        $st = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPeople', 'LocalMachine')
        try { $st.Open('ReadWrite'); $st.Add($cert) } finally { $st.Close() }
    } else {
        $p = $full.Replace("'", "''")
        # Pure .NET in the elevated child too - it must work on whatever powershell.exe module
        # state the machine has (see the section note above).
        $inner = "try { " +
                 "`$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 '$p'; " +
                 "`$s = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPeople', 'LocalMachine'); " +
                 "`$s.Open('ReadWrite'); `$s.Add(`$c); `$s.Close(); exit 0 } catch { exit 7 }"
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))
        try {
            $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -PassThru -Wait -WindowStyle Hidden `
                        -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $enc
        } catch {
            throw "Administrator elevation was cancelled; the signing certificate must be trusted (machine-wide) to install the MSIX."
        }
        if ($proc.ExitCode -ne 0) { throw "Failed to import the signing certificate (elevated import returned $($proc.ExitCode))." }
    }
    if (-not (Test-CertInMachineStore $thumb 'TrustedPeople')) {
        throw "Certificate import did not take effect."
    }
    Write-Ok "trusted ($thumb)"
}

# ---- framework dependency fallback (verbatim from Install-Agentmaster.ps1) ------------------
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

# ⚠ DESIGNED DIVERGENCE from Install-Agentmaster.ps1's Remove-BlockingInstall: no Y/n confirm -
# the user already clicked "Update now" in-app, so a blocking install (an unpackaged / "registered
# loose layout" dev install, or any same-identity install that can't be replaced in place ->
# Add-AppxPackage 0x80073CFB) is removed WITHOUT asking, and its processes are stopped first so the
# registration releases cleanly. Per-user removal needs no admin, and Agentmaster's data lives
# OUTSIDE the package (e.g. %USERPROFILE%\.agentmaster), so it survives.
function Remove-BlockingInstall {
    $pkg = Get-InstalledPackage $Family
    if (-not $pkg) {
        throw "A conflicting Agentmaster install is blocking the update but could not be found to remove automatically. Remove it manually and re-run:  Get-AppxPackage Agentmaster | Remove-AppxPackage"
    }
    $kind = if ($pkg.IsDevelopmentMode) { 'a registered (unpackaged) layout' } else { 'a packaged install' }
    Write-Warn "removing a conflicting existing install ($kind, version $($pkg.Version)); your data is kept"
    Stop-PackageProcesses $pkg
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
    Write-Ok ("removed " + $pkg.PackageFullName)
}

# ⚠ DESIGNED DIVERGENCE from Install-Agentmaster.ps1's Install-Bundle: ALWAYS force-applies
# (-ForceUpdateFromAnyVersion -ForceApplicationShutdown - this IS an update the user asked for)
# and auto-removes a blocker (above) instead of confirming. Same two-failure recovery shape.
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
            Write-Warn 'resolving the framework dependency (VCLibs)...'
            Add-It (Get-VCLibs $Dir)
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

    Write-Step 'Downloading the update'
    Save-File $CerUrl    $cer    ''
    Save-File $BundleUrl $bundle $BundleSha256

    Approve-SigningCert -CerPath $cer

    Wait-ForExit

    Write-Step "Installing $Version"
    Install-Bundle -BundlePath $bundle
    Write-Ok 'installed'

    $now = Get-InstalledPackage $Family
    if (-not $now) { throw 'Install reported success but the package is not registered. See the error above.' }

    Write-Step 'Relaunching Agentmaster'
    Start-Process "shell:appsFolder\$Aumid"
    $shown = if ($Version) { $Version } else { "$($now.Version)" }
    Write-Host ''
    Write-Host "Updated to $shown." -ForegroundColor White
    Start-Sleep -Seconds 2
}

# The app exe inside an unzip/portable folder: Agentmaster.exe (current builds) or
# WindowsTerminal.exe (pre-rename ones). $null when neither is present.
function Get-AppExe {
    param($DirPath)
    foreach ($leaf in 'Agentmaster.exe', 'WindowsTerminal.exe') {
        $p = Join-Path $DirPath $leaf
        if (Test-Path $p) { return $p }
    }
    return $null
}

# The PORTABLE in-place update (mirrors tools\Install-Agentmaster.ps1's Install-Portable core --
# KEEP IN SYNC): download the arch-matched release zip, stop what runs under the portable folder,
# replace its binaries while preserving the exe-side user state, stamp .am-version, relaunch.
function Invoke-PortableUpdate {
    Write-Host ''
    Write-Host "Agentmaster updater - installing $Version (portable, in place)" -ForegroundColor White
    Write-Host ''
    if (-not $ZipUrl -or -not $PortableDir) { throw 'Internal error: the portable update was launched without a zip URL / target folder.' }
    if (-not (Get-AppExe $PortableDir)) {
        throw "The portable folder no longer holds the app exe (Agentmaster.exe): $PortableDir"
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $zip = Join-Path $Dir ([IO.Path]::GetFileName(([Uri]$ZipUrl).AbsolutePath))

    Write-Step 'Downloading the update'
    Save-File $ZipUrl $zip $ZipSha256

    Write-Step 'Extracting'
    $tmp = Join-Path $Dir 'extract'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
    # The zip wraps everything in one folder: agentmaster-<ver> (current releases) or
    # terminal-<ver> (pre-rename ones); a flat zip is tolerated.
    $src = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like 'agentmaster-*' -or $_.Name -like 'terminal-*' } | Select-Object -First 1
    if (-not $src) { $src = Get-Item $tmp }
    if (-not (Get-AppExe $src.FullName)) {
        throw 'The downloaded zip does not look like an Agentmaster portable build (no app exe inside).'
    }

    Wait-ForExit
    Stop-ProcessesUnder $PortableDir

    Write-Step "Installing $Version into $PortableDir"
    # Replace binaries; preserve the user state held next to the exe (Install-Agentmaster.ps1's
    # exact list): settings\ (pre-choice portable WT settings) + profile\ (the self-contained
    # profile) + profile.path (WHICH profile this copy chose) + install.path (the first-launch
    # INSTALL decision -- PortableInstall.h; without it an installed copy would re-ask "where should
    # Agentmaster live?" after every update) + .am-version (refreshed below).
    Get-ChildItem $PortableDir -Force | Where-Object { $_.Name -notin @('settings', 'profile', 'profile.path', 'install.path', '.am-version') } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $src.FullName '*') -Destination $PortableDir -Recurse -Force
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

    # Stamp the new version for the updater's next check. The zip ships its own .am-version (and
    # the copy above laid it down); this explicit write is the belt for updating FROM a zip that
    # predates the stamp, and normalizes a suffixed tag ("v0.8.0-prerelease" -> "0.8.0").
    $num = ($Version -replace '^[vV]', '' -split '-')[0]
    try { ([version]$num).ToString() | Out-File -FilePath (Join-Path $PortableDir '.am-version') -Encoding ascii -Force } catch {}
    Write-Ok 'installed'

    Write-Step 'Relaunching Agentmaster'
    $exe = Get-AppExe $PortableDir
    if ($exe) { Start-Process $exe } else { Write-Warn 'app exe not found after the swap - launch it manually' }
    Write-Host ''
    Write-Host "Updated to $Version." -ForegroundColor White
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
        Write-Warn "Agentmaster ($Family) is not installed - nothing to remove."
        Start-Sleep -Seconds 2
        return
    }
    Write-Step "Removing $($pkg.PackageFullName)"
    Stop-PackageProcesses $pkg
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
    Write-Ok 'uninstalled'
    Write-Info 'Your profile data (e.g. %USERPROFILE%\.agentmaster) was left untouched.'
    Start-Sleep -Seconds 2
}

# Remove an INSTALLED PORTABLE copy (PortableInstall.h's per-user, MSI-like install). Every entry
# is removed ONLY when it points into $PortableDir, so a second install elsewhere (or the MSIX
# package) keeps its own shortcuts / verbs / Apps & Features entry. The exe-side user state
# (settings\ + profile\ + profile.path) and the profile folder itself are never touched.
function Invoke-PortableUninstall {
    Write-Host ''
    Write-Host "Agentmaster uninstaller (portable install)" -ForegroundColor White
    Write-Host ''
    if (-not $PortableDir) { throw 'Internal error: the portable uninstall was launched without a target folder.' }
    $dir = [IO.Path]::GetFullPath($PortableDir).TrimEnd('\')
    $under = { param($p) $p -and ([IO.Path]::GetFullPath($p).TrimEnd('\') + '\').StartsWith($dir + '\', [StringComparison]::OrdinalIgnoreCase) }

    Wait-ForExit
    Stop-ProcessesUnder $dir

    Write-Step 'Removing shortcuts'
    $sh = New-Object -ComObject WScript.Shell
    foreach ($folder in @([Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('DesktopDirectory'))) {
        if (-not $folder) { continue }
        $lnk = Join-Path $folder 'Agentmaster.lnk'
        if (Test-Path -LiteralPath $lnk) {
            $target = ''
            try { $target = $sh.CreateShortcut($lnk).TargetPath } catch {}
            if (& $under $target) { Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue; Write-Ok "removed $lnk" }
            else { Write-Info "kept $lnk (points elsewhere: $target)" }
        }
    }

    Write-Step 'Removing the right-click menu'
    foreach ($base in 'Directory', 'Directory\Background', 'Drive') {
        $key = "HKCU:\Software\Classes\$base\shell\Agentmaster"
        if (Test-Path -LiteralPath $key) {
            $cmd = ''
            try { $cmd = (Get-ItemProperty -LiteralPath "$key\command" -ErrorAction SilentlyContinue).'(default)' } catch {}
            if (-not $cmd -or $cmd -like "*$dir*") { Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue; Write-Ok "removed $key" }
            else { Write-Info "kept $key (points elsewhere)" }
        }
    }

    Write-Step 'Removing the Apps & Features entry'
    $uk = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster'
    if (Test-Path -LiteralPath $uk) {
        $loc = ''
        try { $loc = (Get-ItemProperty -LiteralPath $uk -ErrorAction SilentlyContinue).InstallLocation } catch {}
        if (-not $loc -or ([IO.Path]::GetFullPath($loc).TrimEnd('\') -ieq $dir)) { Remove-Item -LiteralPath $uk -Recurse -Force -ErrorAction SilentlyContinue; Write-Ok 'removed' }
        else { Write-Info "kept (it describes another install at $loc)" }
    }

    Write-Step 'Removing the PATH entry'
    try {
        $envKey = Get-Item -LiteralPath 'HKCU:\Environment'
        $raw = $envKey.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
        if ($raw) {
            $kept = @($raw -split ';' | Where-Object { $_ -and ($_.Trim().Trim('"').TrimEnd('\') -ine $dir) })
            $new = ($kept -join ';')
            if ($new -ne $raw) {
                Set-ItemProperty -LiteralPath 'HKCU:\Environment' -Name 'Path' -Value $new -Type ExpandString
                # Tell running shells / Explorer the environment changed (what setx does).
                $sig = '[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);'
                $u32 = Add-Type -MemberDefinition $sig -Name 'AmEnvBroadcast' -Namespace 'Agentmaster' -PassThru -ErrorAction SilentlyContinue
                if ($u32) { [UIntPtr]$r = [UIntPtr]::Zero; $null = $u32::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 3000, [ref]$r) }
                Write-Ok 'removed'
            }
        }
    } catch { Write-Warn "PATH entry not updated: $($_.Exception.Message)" }

    Write-Step "Removing the binaries under $dir"
    $keep = @('settings', 'profile', 'profile.path')
    Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -notin $keep } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    $left = @(Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue)
    if ($left.Count -eq 0) { Remove-Item -LiteralPath $dir -Force -ErrorAction SilentlyContinue; Write-Ok 'removed' }
    else { Write-Ok "removed (kept your exe-side state: $(($left | ForEach-Object { $_.Name }) -join ', '))" }

    Write-Host ''
    Write-Host 'Agentmaster was uninstalled.' -ForegroundColor White
    Write-Info 'Your profile data (sessions, settings, window layouts) was left untouched.'
    Start-Sleep -Seconds 3
}

# ============================ main ============================
try {
    Initialize-Net
    if ($Uninstall) { Invoke-Uninstall } elseif ($UninstallPortable) { Invoke-PortableUninstall } elseif ($Portable) { Invoke-PortableUpdate } else { Invoke-Install }
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
