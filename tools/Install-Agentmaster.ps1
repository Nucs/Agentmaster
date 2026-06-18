<#
.SYNOPSIS
    Install or upgrade Agentmaster from its GitHub Releases - fully automated.

.DESCRIPTION
    One script that does everything the manual "download the .cer + .msixbundle, trust the
    cert, Add-AppxPackage" dance does, and makes UPGRADES a no-op to run:

      * Resolves the release to install (latest by default, or -Version X.Y.Z, or -Prerelease).
      * Compares it with what is already installed and SKIPS if you are current (unless -Force).
      * Downloads the signed .msixbundle + Agentmaster.cer (cached + SHA-256 verified).
      * Trusts the self-signed certificate (LocalMachine\TrustedPeople) - elevating ONLY for
        that one step, and ONLY when the cert is not already trusted (so upgrades are prompt-free).
      * Installs / upgrades the package, auto-resolving the VCLibs framework dependency if missing.
      * If a conflicting earlier install (e.g. a registered / "loose layout" unpackaged dev install)
        blocks deployment, offers to uninstall it and then continues. Your data is preserved.

    Alternatively, -Portable installs/upgrades the cert-free, self-contained portable build into
    a folder (default %LOCALAPPDATA%\Programs\Agentmaster) with a Start-menu shortcut - no admin,
    no certificate, nothing registered.

    Safe to re-run anytime: it is the upgrade path.

.PARAMETER Version
    Specific version to install, e.g. "0.4.0" (a leading "v" is fine). Default: the latest release.

.PARAMETER Prerelease
    Consider pre-releases when picking the newest version (ignored when -Version is given).

.PARAMETER Portable
    Install/upgrade the portable build (no cert, no admin) instead of the MSIX package.

.PARAMETER Force
    Reinstall even if the installed version is the same or newer. For MSIX this also force-closes
    a running Agentmaster so the upgrade applies immediately.

.PARAMETER Launch
    Start Agentmaster after a successful install/upgrade.

.PARAMETER Uninstall
    Remove the installed Agentmaster (MSIX by default; add -Portable to remove the portable folder).
    Your profile data (e.g. %USERPROFILE%\.agentmaster) is never touched.

.PARAMETER InstallDir
    Portable install location. Default: %LOCALAPPDATA%\Programs\Agentmaster.

.PARAMETER DownloadDir
    Where to cache downloads. Default: %TEMP%\Agentmaster-install.

.PARAMETER Repo
    owner/name of the GitHub repo. Default: Nucs/Agentmaster.

.PARAMETER GitHubToken
    Optional token to lift the unauthenticated GitHub API rate limit. Defaults to $env:GITHUB_TOKEN.

.EXAMPLE
    .\Install-Agentmaster.ps1
    Install or upgrade to the latest release (MSIX).

.EXAMPLE
    .\Install-Agentmaster.ps1 -Version 0.4.0 -Launch
    Install a specific version and start it.

.EXAMPLE
    .\Install-Agentmaster.ps1 -Portable -Launch
    Install/upgrade the cert-free portable build and run it.

.EXAMPLE
    # Run straight from the web (latest, MSIX):
    irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1 | iex

    # ... or with arguments:
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Portable -Launch

.NOTES
    Agentmaster is a fork of Windows Terminal. The released MSIX is self-signed, so trusting its
    certificate requires administrator rights once; the portable build needs neither.
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]   $Version,
    [switch]   $Prerelease,
    [switch]   $Portable,
    [switch]   $Force,
    [switch]   $Launch,
    [switch]   $Uninstall,
    [string]   $InstallDir,
    [string]   $DownloadDir,
    [string]   $Repo        = 'Nucs/Agentmaster',
    [string]   $GitHubToken = $env:GITHUB_TOKEN
)

$ErrorActionPreference = 'Stop'

# ---- identity constants (stable: the PFN hash derives from Publisher CN=Agentmaster) ----------
$PackageName = 'Agentmaster'
$FamilyName  = 'Agentmaster_56k4f06dsfp9r'
$Aumid       = "$FamilyName!App"

# ---- tiny console UI -------------------------------------------------------------------------
function Write-Step { param($m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "    $m" -ForegroundColor Yellow }
function Write-Info { param($m) Write-Host "    $m" -ForegroundColor DarkGray }

# ---- environment helpers ---------------------------------------------------------------------
function Test-Windows {
    if ($env:OS -ne 'Windows_NT') { throw "Agentmaster runs on Windows only." }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-OSArch {
    switch ($env:PROCESSOR_ARCHITECTURE) {
        'ARM64' { 'arm64' }
        'AMD64' { 'x64' }
        'x86'   { 'x86' }
        default { 'x64' }
    }
}

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

# ---- GitHub release resolution ---------------------------------------------------------------
function Get-GitHubHeaders {
    $h = @{ 'User-Agent' = 'Agentmaster-Installer'; 'Accept' = 'application/vnd.github+json' }
    if ($GitHubToken) { $h['Authorization'] = "Bearer $GitHubToken" }
    return $h
}

function Get-Release {
    param($Headers)
    $base = "https://api.github.com/repos/$Repo/releases"
    if ($Version) {
        $tag = if ($Version -match '^v') { $Version } else { "v$Version" }
        Write-Step "Resolving release $tag from $Repo"
        try {
            return Invoke-RestMethod -Uri "$base/tags/$tag" -Headers $Headers
        } catch {
            throw "Release '$tag' not found in $Repo. Check the version, or list with: gh release list -R $Repo"
        }
    }
    if ($Prerelease) {
        Write-Step "Resolving newest release (incl. pre-releases) from $Repo"
        $all = Invoke-RestMethod -Uri "$base`?per_page=20" -Headers $Headers
        $r = $all | Where-Object { -not $_.draft } | Select-Object -First 1
        if (-not $r) { throw "No releases found in $Repo." }
        return $r
    }
    Write-Step "Resolving latest release from $Repo"
    return Invoke-RestMethod -Uri "$base/latest" -Headers $Headers
}

function Get-AssetVersion {
    param($AssetName, $TagName)
    if ($AssetName -match '_(\d+\.\d+\.\d+\.\d+)') { return [version]$Matches[1] }
    return [version]("{0}.0" -f ($TagName -replace '^v', ''))
}

# ---- downloading -----------------------------------------------------------------------------
function Test-Digest {
    param($Path, $Asset)
    $digest = $null
    if ($Asset.PSObject.Properties.Name -contains 'digest') { $digest = $Asset.digest }
    if (-not $digest) { return $true }   # nothing to verify against
    if ($digest -match '^sha256:(?<h>[0-9a-fA-F]{64})$') {
        $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
        return ($have -ieq $Matches.h)
    }
    return $true
}

function Save-Asset {
    param($Asset, $Dir)
    $dest = Join-Path $Dir $Asset.name
    if ((Test-Path $dest) -and ((Get-Item $dest).Length -eq $Asset.size) -and (Test-Digest $dest $Asset)) {
        Write-Ok ("cached  {0}" -f $Asset.name)
        return $dest
    }
    $mb = '{0:N1} MB' -f ($Asset.size / 1MB)
    Write-Ok ("download {0}  ({1})" -f $Asset.name, $mb)
    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Asset.browser_download_url -OutFile $dest -UseBasicParsing -Headers @{ 'User-Agent' = 'Agentmaster-Installer' }
    } finally { $ProgressPreference = $old }
    if (-not (Test-Digest $dest $Asset)) { throw "Checksum mismatch for $($Asset.name) - download corrupt; re-run." }
    return $dest
}

function Find-Asset {
    param($Release, [string]$Pattern, [switch]$Like)
    $a = $Release.assets | Where-Object { if ($Like) { $_.name -like $Pattern } else { $_.name -match $Pattern } } | Select-Object -First 1
    return $a
}

# ---- certificate trust (the only step that needs admin) --------------------------------------
function Approve-SigningCert {
    param($CerPath)
    $full = (Resolve-Path -LiteralPath $CerPath).Path
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $full
    $thumb = $cert.Thumbprint
    if (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb") {
        Write-Ok "signing cert already trusted ($thumb)"
        return
    }
    Write-Step "Trusting the signing certificate (one-time, requires administrator)"
    if (Test-Admin) {
        Import-Certificate -FilePath $full -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
    } else {
        $p = $full.Replace("'", "''")
        $inner = "try { Import-Certificate -FilePath '$p' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' -ErrorAction Stop | Out-Null; exit 0 } catch { exit 7 }"
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))
        try {
            $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -PassThru -Wait -WindowStyle Hidden `
                        -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $enc
        } catch {
            throw "Administrator elevation was cancelled. The signing certificate must be trusted to install the MSIX. Tip: re-run with -Portable for a cert-free, no-admin install."
        }
        if ($proc.ExitCode -ne 0) { throw "Failed to import the signing certificate (elevated import returned $($proc.ExitCode))." }
    }
    if (-not (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb")) {
        throw "Certificate import did not take effect."
    }
    Write-Ok "trusted ($thumb)"
}

# ---- framework dependency fallback (offline / Store-less machines) ----------------------------
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

function Resolve-MissingDeps {
    param($Message, $Dir)
    $deps = @()
    if ($Message -match 'VCLibs') {
        try { $deps += Get-VCLibs $Dir } catch { Write-Warn "could not fetch VCLibs: $($_.Exception.Message)" }
    }
    if ($Message -match 'Microsoft\.UI\.Xaml') {
        Write-Warn "This build needs the 'Microsoft.UI.Xaml' framework, which can't be auto-fetched reliably."
        Write-Warn "Install it from the Microsoft Store, or just use the cert-free portable build: re-run with -Portable."
    }
    return , $deps
}

# A conflicting existing registration (an unpackaged / "registered loose layout" dev install,
# or any install of the same identity that can't be replaced in place) makes Add-AppxPackage fail
# with 0x80073CFB. Offer to uninstall it, then let the caller retry. Per-user removal needs no admin,
# and Agentmaster's data lives OUTSIDE the package (e.g. %USERPROFILE%\.agentmaster), so it survives.
function Remove-BlockingInstall {
    param([bool]$AutoYes)
    $pkg = Get-InstalledMsix
    if (-not $pkg) {
        Write-Warn "A conflicting Agentmaster registration is present but not visible to remove automatically."
        Write-Warn "Remove it manually, then re-run:  Get-AppxPackage Agentmaster | Remove-AppxPackage"
        return $false
    }
    $kind = if ($pkg.IsDevelopmentMode) { 'a registered (unpackaged) layout' } else { 'a packaged install' }
    Write-Warn "An existing Agentmaster install is blocking this one and must be removed first:"
    Write-Info "  $($pkg.PackageFullName)"
    Write-Info "  ($kind, version $($pkg.Version))"
    $go = $AutoYes
    if (-not $go) {
        $ans = Read-Host "  Uninstall it and continue? Your data (e.g. %USERPROFILE%\.agentmaster) is kept [Y/n]"
        $go = [string]::IsNullOrWhiteSpace($ans) -or $ans -match '^(y|yes)$'
    }
    if (-not $go) { Write-Warn "Left the existing install in place; nothing changed."; return $false }
    Write-Step "Uninstalling the conflicting install"
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
    Write-Ok "uninstalled"
    return $true
}

function Install-Bundle {
    param($BundlePath, $Dir, [bool]$ForceFlag)
    $splat = @{ Path = $BundlePath; ErrorAction = 'Stop' }
    if ($ForceFlag) { $splat['ForceUpdateFromAnyVersion'] = $true; $splat['ForceApplicationShutdown'] = $true }
    try {
        Add-AppxPackage @splat
        return
    } catch {
        $msg = $_.Exception.Message

        # Missing framework dependency -> fetch it and retry.
        if ($msg -match '0x80073CF3' -or $msg -match 'dependency') {
            Write-Warn "Resolving missing framework dependencies..."
            $deps = Resolve-MissingDeps -Message $msg -Dir $Dir
            if ($deps -and $deps.Count -gt 0) {
                $splat['DependencyPath'] = $deps
                Add-AppxPackage @splat   # retry with explicit dependencies
                return
            }
        }

        # Conflicting existing install (unpackaged/registered, or otherwise unreplaceable) ->
        # offer to uninstall it, then retry. -Force auto-confirms.
        if ($msg -match '0x80073CFB' -or $msg -match 'already installed' -or
            $msg -match 'cannot replace' -or $msg -match 'unpackaged') {
            if (Remove-BlockingInstall -AutoYes:$ForceFlag) {
                Add-AppxPackage @splat   # retry after removing the blocker
                return
            }
        }

        throw
    }
}

# ---- installed-state queries -----------------------------------------------------------------
function Get-InstalledMsix {
    Get-AppxPackage | Where-Object { $_.PackageFamilyName -eq $FamilyName } | Select-Object -First 1
}

# ---- portable -------------------------------------------------------------------------------
function Stop-RunningUnder {
    param($Dir)
    $procs = Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) }
    foreach ($p in $procs) {
        Write-Warn "closing running portable instance (pid $($p.ProcessId))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($procs) { Start-Sleep -Milliseconds 700 }
}

function New-StartMenuShortcut {
    param($Target, $Name)
    try {
        $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) "$Name.lnk"
        $sh = New-Object -ComObject WScript.Shell
        $s = $sh.CreateShortcut($lnk)
        $s.TargetPath = $Target
        $s.WorkingDirectory = Split-Path $Target
        $s.Description = 'Agentmaster - manage multiple Claude Code sessions'
        $s.Save()
        Write-Ok "Start-menu shortcut: $lnk"
    } catch { Write-Warn "could not create Start-menu shortcut: $($_.Exception.Message)" }
}

function Install-Portable {
    param($Release, $RelVer, $Dir, $Cache)
    $arch = Get-OSArch
    $zip = Find-Asset $Release "_${arch}\.zip$"
    if (-not $zip) { throw "Release $($Release.tag_name) has no portable $arch zip." }

    $marker = Join-Path $Dir '.am-version'
    if ((Test-Path $marker) -and -not $Force) {
        $cur = [version]((Get-Content -LiteralPath $marker -Raw).Trim())
        if ($cur -ge $RelVer) { Write-Ok "Portable already up to date (installed $cur >= release $RelVer). Use -Force to reinstall."; return $false }
        Write-Step "Upgrading portable $cur -> $RelVer"
    } elseif (Test-Path $Dir) {
        Write-Step "Reinstalling portable $RelVer into $Dir"
    } else {
        Write-Step "Installing portable $RelVer into $Dir"
    }

    $zipPath = Save-Asset $zip $Cache
    Stop-RunningUnder $Dir

    $tmp = Join-Path $Cache ('extract-' + [IO.Path]::GetFileNameWithoutExtension($zip.name))
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -LiteralPath $zipPath -DestinationPath $tmp -Force
    $src = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like 'terminal-*' } | Select-Object -First 1
    if (-not $src) { $src = Get-Item $tmp }   # fall back to flat layout

    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    # Replace binaries; preserve user state held next to the exe (.portable: settings\ + profile\).
    Get-ChildItem $Dir -Force | Where-Object { $_.Name -notin @('settings', 'profile', '.am-version') } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $src.FullName '*') -Destination $Dir -Recurse -Force
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

    $RelVer.ToString() | Out-File -FilePath $marker -Encoding ascii -Force
    New-StartMenuShortcut (Join-Path $Dir 'WindowsTerminal.exe') 'Agentmaster (Portable)'
    Write-Ok "Portable installed at $Dir"
    return $true
}

# ---- uninstall -------------------------------------------------------------------------------
function Uninstall-All {
    param($Dir)
    $did = $false
    if ($Portable) {
        if (Test-Path $Dir) {
            Stop-RunningUnder $Dir
            Write-Step "Removing portable install at $Dir"
            Remove-Item -Recurse -Force $Dir
            $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Agentmaster (Portable).lnk'
            if (Test-Path $lnk) { Remove-Item -Force $lnk }
            Write-Ok "Portable removed."
            $did = $true
        } else { Write-Warn "No portable install found at $Dir." }
    } else {
        $pkg = Get-InstalledMsix
        if ($pkg) {
            Write-Step "Removing $($pkg.PackageFullName)"
            Remove-AppxPackage -Package $pkg.PackageFullName
            Write-Ok "Removed."
            $did = $true
        } else { Write-Warn "Agentmaster (MSIX) is not installed." }
    }
    if ($did) { Write-Info "Your profile data (e.g. %USERPROFILE%\.agentmaster) was left untouched." }
}

# ============================ main ============================================================
try {
    Test-Windows
    Initialize-Net
    Import-AppxModule

    if (-not $InstallDir)  { $InstallDir  = Join-Path $env:LOCALAPPDATA 'Programs\Agentmaster' }
    if (-not $DownloadDir) { $DownloadDir = Join-Path $env:TEMP 'Agentmaster-install' }
    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null

    Write-Host ""
    Write-Host "Agentmaster installer" -ForegroundColor White
    Write-Host ""

    if ($Uninstall) { Uninstall-All -Dir $InstallDir; return }

    $headers = Get-GitHubHeaders
    $release = Get-Release -Headers $headers
    $tag     = $release.tag_name

    if ($Portable) {
        $zipAsset = Find-Asset $release "_$([regex]::Escape((Get-OSArch)))\.zip$"
        $relVer   = Get-AssetVersion -AssetName ($zipAsset.name) -TagName $tag
        Write-Ok "release $tag  (version $relVer)"
        $changed = Install-Portable -Release $release -RelVer $relVer -Dir $InstallDir -Cache $DownloadDir
        Write-Host ""
        Write-Host "Done." -ForegroundColor White
        Write-Info "Launch: `"$InstallDir\WindowsTerminal.exe`"  (or the 'Agentmaster (Portable)' Start-menu entry)"
        if ($Launch -and (Test-Path (Join-Path $InstallDir 'WindowsTerminal.exe'))) {
            Write-Step "Launching Agentmaster"
            Start-Process (Join-Path $InstallDir 'WindowsTerminal.exe')
        }
        return
    }

    # ---- MSIX path ----
    $bundleAsset = Find-Asset $release '*.msixbundle' -Like
    if (-not $bundleAsset) { throw "Release $tag has no .msixbundle asset." }
    $cerAsset = Find-Asset $release '*.cer' -Like
    if (-not $cerAsset) { throw "Release $tag has no .cer (signing certificate) asset." }

    $relVer = Get-AssetVersion -AssetName $bundleAsset.name -TagName $tag
    Write-Ok "release $tag  (version $relVer)"

    $installed = Get-InstalledMsix
    if ($installed) {
        $iv = [version]$installed.Version
        if (($iv -ge $relVer) -and -not $Force) {
            Write-Ok "Already up to date (installed $iv >= release $relVer). Use -Force to reinstall."
            if ($Launch) { Write-Step "Launching Agentmaster"; Start-Process "shell:appsFolder\$Aumid" }
            return
        }
        Write-Step "Upgrading $iv -> $relVer"
    } else {
        Write-Step "Installing $relVer"
    }

    $cerPath    = Save-Asset $cerAsset    $DownloadDir
    $bundlePath = Save-Asset $bundleAsset $DownloadDir

    Approve-SigningCert -CerPath $cerPath

    Write-Step "Installing the package"
    Install-Bundle -BundlePath $bundlePath -Dir $DownloadDir -ForceFlag ([bool]$Force)

    $now = Get-InstalledMsix
    if (-not $now) { throw "Install reported success but the package isn't registered. See the error above." }
    Write-Host ""
    Write-Host "Done - Agentmaster $($now.Version) is installed." -ForegroundColor White
    Write-Info "Launch: run 'agentmaster' or use the 'Agentmaster' Start-menu entry."
    if ($installed -and -not $Force) {
        Write-Info "If Agentmaster is currently running, restart it to pick up the new version."
    }
    Write-Info "On first launch it asks which profile folder to use (Production / Development / Browse)."

    if ($Launch) { Write-Step "Launching Agentmaster"; Start-Process "shell:appsFolder\$Aumid" }
}
catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "       For a cert-free, no-admin alternative, add -Portable." -ForegroundColor DarkGray
    # Only set a failure exit code when invoked as a downloaded .ps1 file. A one-liner
    # (irm|iex or & ([scriptblock]::Create(...))) shares the caller's shell, where 'exit'
    # would CLOSE their window — so signal failure by message + LASTEXITCODE only.
    $global:LASTEXITCODE = 1
    if ($PSCommandPath) { exit 1 }
}
