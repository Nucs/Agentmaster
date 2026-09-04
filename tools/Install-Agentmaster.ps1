# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Install or upgrade Agentmaster from its GitHub Releases - fully automated.

.DESCRIPTION
    One script that does everything the manual "download the .cer + .msixbundle, trust the
    cert, Add-AppxPackage" dance does, and makes UPGRADES a no-op to run:

      * Resolves the release to install (latest by default, or -Version X.Y.Z, or -Prerelease,
        or -Nightly for unstable development builds).
      * Compares it with what is already installed and SKIPS if you are current (unless -Force).
      * Downloads the signed .msixbundle + Agentmaster.cer (cached + SHA-256 verified).
      * Trusts the self-signed certificate (LocalMachine\TrustedPeople) - the ONE step that
        needs administrator rights, and only when the cert is not already machine-trusted
        (re-runs of the same release, or releases signed with the same cert, are prompt-free).
        As administrator it is silent. NOT running as administrator, the script ASKS first:
          [A] Administrator install - elevate: one UAC prompt trusts the cert machine-wide,
              then the MSIX still installs for the CURRENT user (preferred; the default).
          [U] User install - no admin at all: falls back to the cert-free, self-contained
              PORTABLE build (exactly what -Portable installs).
          [C] Cancel - install nothing.
        A declined UAC re-offers the question instead of failing. -Elevate pre-answers [A]
        and -Portable pre-answers [U]; a non-interactive session behaves like -Elevate (the
        classic straight-to-UAC behavior, so automation never hangs on a prompt).
      * Installs / upgrades the package, auto-resolving the VCLibs framework dependency if missing.
      * If a conflicting earlier install (e.g. a registered / "loose layout" unpackaged dev install)
        blocks deployment, offers to uninstall it and then continues. Your data is preserved.

    Alternatively, -Portable installs/upgrades the cert-free, self-contained portable build into
    a folder (default %LOCALAPPDATA%\Programs\Agentmaster) with a Start-menu shortcut - no admin,
    no certificate, nothing registered.

    Safe to re-run anytime: it is the upgrade path.

.PARAMETER Version
    Specific version to install, e.g. "0.4.0" (a leading "v" is fine). Default: the latest release.
    A nightly is named by its FULL suffixed version (e.g. "0.6.10-prerelease-nightly" - the tag
    minus the leading v); an explicit -Version always wins, no channel switch needed.

.PARAMETER Prerelease
    Consider pre-releases when picking the newest version (ignored when -Version is given).
    NIGHTLY builds are NOT considered - they are a tier below pre-release (see -Nightly).

.PARAMETER Nightly
    Also consider NIGHTLY builds when picking the newest version (ignored when -Version is given).
    A nightly is an UNSTABLE development build (release tag contains "nightly", published as a
    GitHub prerelease) that may have memory leaks, CPU issues, and crashes - install one only to
    help test. Mirrors the in-app updater's channel model: each switch admits only its own tier
    (-Nightly alone = nightlies + stable; -Prerelease alone = betas + stable; both = everything).

.PARAMETER Portable
    Install/upgrade the portable build (no cert, no admin) instead of the MSIX package.
    This is also what the non-admin install-mode question's "User install" choice lands on.

.PARAMETER Elevate
    When the MSIX certificate needs machine-wide trust and the session is not elevated, skip
    the install-mode question and go straight to the UAC elevation (the classic behavior).
    The opposite pre-answer is -Portable (the no-admin user install).

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
    .\Install-Agentmaster.ps1 -Nightly
    Install/upgrade to the newest build INCLUDING unstable nightlies (the testing channel -
    may have memory leaks, CPU issues, and crashes).

.EXAMPLE
    .\Install-Agentmaster.ps1 -Elevate
    Non-admin shell, no questions asked: go straight to the one UAC prompt and install the MSIX.

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
    [switch]   $Nightly,
    [switch]   $Portable,
    [switch]   $Elevate,
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

function Test-CanPrompt {
    # Only ask questions where a human can answer them: an interactive user session whose host
    # was not started -NonInteractive ('-non' is the shortest unambiguous prefix PowerShell
    # itself accepts for that switch, so match any spelling of it).
    if (-not [Environment]::UserInteractive) { return $false }
    if ([Environment]::GetCommandLineArgs() -match '(?i)^[-/]non') { return $false }
    return $true
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

# A NIGHTLY is an unstable development build whose release TAG contains "nightly"
# (e.g. v0.6.10-prerelease-nightly; published as a GitHub prerelease). The TAG is the
# authoritative signal - same contract as the in-app updater (Updater.h IsNightlyTag).
function Test-NightlyTag {
    param($TagName)
    return ("$TagName" -match '(?i)nightly')
}

function Get-Release {
    param($Headers)
    $base = "https://api.github.com/repos/$Repo/releases"
    if ($Version) {
        # Explicit ask wins - naming a nightly version installs that nightly, no switch needed.
        $tag = if ($Version -match '^v') { $Version } else { "v$Version" }
        Write-Step "Resolving release $tag from $Repo"
        try {
            return Invoke-RestMethod -Uri "$base/tags/$tag" -Headers $Headers
        } catch {
            throw "Release '$tag' not found in $Repo. Check the version, or list with: gh release list -R $Repo"
        }
    }
    if ($Prerelease -or $Nightly) {
        $incl = if ($Prerelease -and $Nightly) { 'pre-releases + nightlies' }
                elseif ($Nightly)              { 'nightlies' }
                else                           { 'pre-releases' }
        Write-Step "Resolving newest release (incl. $incl) from $Repo"
        $all = Invoke-RestMethod -Uri "$base`?per_page=20" -Headers $Headers
        # The in-app updater's channel model (Updater.h ReleaseAllowedOnChannel): stable is always
        # eligible; a NIGHTLY (by tag) only under -Nightly - regardless of the prerelease flag, so
        # -Prerelease alone NEVER installs a nightly; any other prerelease only under -Prerelease.
        # First eligible non-draft = the newest release on the requested channel.
        $r = $all | Where-Object {
            if ($_.draft) { return $false }
            if (Test-NightlyTag $_.tag_name) { return [bool]$Nightly }
            if ($_.prerelease) { return [bool]$Prerelease }
            return $true
        } | Select-Object -First 1
        if (-not $r) { throw "No releases found in $Repo on the requested channel." }
        return $r
    }
    Write-Step "Resolving latest release from $Repo"
    $r = Invoke-RestMethod -Uri "$base/latest" -Headers $Headers
    if ($r -and (Test-NightlyTag $r.tag_name)) {
        # Belt (mirrors the in-app updater's /latest guard): nightlies ship as prereleases, which
        # /latest excludes - one surfacing here is a mis-published release and must not land on the
        # default channel. Fall back to the newest genuinely-stable release.
        Write-Warn "latest ($($r.tag_name)) is a NIGHTLY (unstable dev build) - skipped on the default channel (use -Nightly to opt in)"
        $all = Invoke-RestMethod -Uri "$base`?per_page=20" -Headers $Headers
        $r = $all | Where-Object { -not $_.draft -and -not $_.prerelease -and -not (Test-NightlyTag $_.tag_name) } | Select-Object -First 1
        if (-not $r) { throw "No stable (non-nightly) release found in $Repo. Use -Version <x.y.z>, -Prerelease, or -Nightly." }
    }
    return $r
}

function Get-AssetVersion {
    param($AssetName, $TagName)
    if ($AssetName -match '_(\d+\.\d+\.\d+\.\d+)') { return [version]$Matches[1] }
    return [version]("{0}.0" -f ($TagName -replace '^v', ''))
}

# ---- downloading -----------------------------------------------------------------------------
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

function Test-Digest {
    param($Path, $Asset)
    $digest = $null
    if ($Asset.PSObject.Properties.Name -contains 'digest') { $digest = $Asset.digest }
    if (-not $digest) { return $true }   # nothing to verify against
    if ($digest -match '^sha256:(?<h>[0-9a-fA-F]{64})$') {
        $have = Get-FileSha256 $Path
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
# AppX deployment validates the package signature in SYSTEM context, so the cert must be trusted
# MACHINE-wide (LocalMachine TrustedPeople or Root) - a CurrentUser store never satisfies it.
# That is WHY there is no admin-free way to trust it, and why the no-admin fallback is portable.
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

# ---- the non-admin install-mode question -----------------------------------------------------
# The happy design: machine-level (administrator) install is PREFERRED, user-level (portable)
# is the safe fallback that must always exist, cancel changes nothing. Only asked when it
# actually matters: MSIX path + not admin + this release's cert not yet machine-trusted.
function Read-InstallModeChoice {
    param($PortableDir)
    Write-Host ""
    Write-Host "    You are not running as administrator." -ForegroundColor Yellow
    Write-Host "    The MSIX package is self-signed: its certificate must be trusted MACHINE-wide once," -ForegroundColor Yellow
    Write-Host "    which needs admin rights. (The app itself installs per-user either way.)" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "      [A] Administrator install " -NoNewline -ForegroundColor White
    Write-Host "- elevate now: one UAC prompt trusts the certificate,"
    Write-Host "          then the MSIX package installs for this user.  (preferred; default)"
    Write-Host "      [U] User install " -NoNewline -ForegroundColor White
    Write-Host "- no admin, no certificate: the self-contained PORTABLE build"
    Write-Host "          into `"$PortableDir`" + a Start-menu shortcut."
    Write-Host "      [C] Cancel " -NoNewline -ForegroundColor White
    Write-Host "- install nothing."
    Write-Host ""
    while ($true) {
        try { $ans = Read-Host "    Choice [A]dmin / [U]ser / [C]ancel  (Enter = A)" }
        catch { return 'admin' }   # the host can't prompt after all -> the pre-question behavior
        switch -Regex (("$ans").Trim()) {
            '^$|^(a|admin|e|elevate)$' { return 'admin' }
            '^(u|user|p|portable)$'    { return 'user' }
            '^(c|cancel|n|no|q|quit)$' { return 'cancel' }
        }
        Write-Warn "unrecognized answer '$ans' - type A, U, or C"
    }
}

# Ask + act. Returns 'admin' (caller proceeds with the elevating MSIX path), 'done' (the
# portable fallback was installed - caller stops), or 'cancel' (caller stops, nothing changed).
function Invoke-InstallModeChoice {
    param($Release, $Cache, $PortableDir)
    switch (Read-InstallModeChoice -PortableDir $PortableDir) {
        'user' {
            Write-Step "User install - installing the cert-free portable build instead of the MSIX"
            Invoke-PortableFlow -Release $Release -Cache $Cache -Dir $PortableDir
            return 'done'
        }
        'cancel' {
            Write-Warn "Cancelled - nothing was installed."
            return 'cancel'
        }
        default { return 'admin' }
    }
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
# (am-update.ps1's twin deliberately diverges: it never asks - see the note there.)
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

# (am-update.ps1's twin deliberately diverges: always force-applies + auto-removes a blocker -
# it runs after the user clicked "Update now" in-app. Same two-failure recovery shape.)
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
    # Kill ONLY the GUI process (Agentmaster.exe / a pre-rename WindowsTerminal.exe) — NEVER its
    # OpenConsole.exe ConPTY hosts. A host hit by Stop-Process dies without its graceful shutdown (the
    # CTRL_CLOSE_EVENT broadcast to the tab's pwsh + claude.exe + MCP children), which leaves every
    # session of that tab running forever with a DEAD console (2026-09-04: 35 pwsh+claude pairs and
    # ~140 node/cmd children from one such kill, alive six days). Killing the GUI alone closes its pipe
    # handles; each host then shuts its clients down itself and exits — we WAIT for that below so the
    # binaries are free before the swap (a host locks OpenConsole.exe in $Dir).
    $gui = Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe' OR Name='WindowsTerminal.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) }
    foreach ($p in $gui) {
        Write-Warn "closing running portable instance (pid $($p.ProcessId))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if (-not $gui) { return }
    # Wait for the ConPTY hosts under $Dir to drain (each ends its clients — up to ~5 s per stubborn
    # client — then exits). 30 s is generous; whatever is still there after that is wedged, and only
    # then is it killed as the last resort (logged: that tab's clients WILL be orphaned).
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500
        $hosts = Get-CimInstance Win32_Process -Filter "Name='OpenConsole.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) }
    } while ($hosts -and (Get-Date) -lt $deadline)
    foreach ($p in $hosts) {
        Write-Warn "ConPTY host pid $($p.ProcessId) did not exit in 30 s; killing it (its tab's session processes may be orphaned)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 700
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
    # The zip's wrapper folder: agentmaster-<ver> (current releases) or terminal-<ver> (pre-rename
    # ones -- keep matching so -Version can still install an older release).
    $src = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -like 'agentmaster-*' -or $_.Name -like 'terminal-*' } | Select-Object -First 1
    if (-not $src) { $src = Get-Item $tmp }   # fall back to flat layout

    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    # Replace binaries; preserve user state held next to the exe (.portable: settings\ + profile\ +
    # profile.path — the exe-side pointer remembering WHICH profile this copy chose at first launch;
    # wiping it would re-prompt every upgrade and forget a custom/absolute choice — and install.path,
    # the first-launch INSTALL decision (PortableInstall.h): without it an installed copy re-asks
    # "where should Agentmaster live?" after every upgrade).
    Get-ChildItem $Dir -Force | Where-Object { $_.Name -notin @('settings', 'profile', 'profile.path', 'install.path', '.am-version') } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $src.FullName '*') -Destination $Dir -Recurse -Force
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

    $RelVer.ToString() | Out-File -FilePath $marker -Encoding ascii -Force
    # The app exe: Agentmaster.exe (current releases) or WindowsTerminal.exe (a pinned pre-rename
    # -Version install) - shortcut + launch resolve whichever the zip actually shipped.
    $appExe = @('Agentmaster.exe', 'WindowsTerminal.exe') | ForEach-Object { Join-Path $Dir $_ } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $appExe) { $appExe = Join-Path $Dir 'Agentmaster.exe' }
    New-StartMenuShortcut $appExe 'Agentmaster (Portable)'
    Write-Ok "Portable installed at $Dir"
    return $true
}

# The whole portable journey (asset -> install -> how-to-launch epilogue) - shared by -Portable
# and by the non-admin install-mode question's "User install" fallback.
function Invoke-PortableFlow {
    param($Release, $Cache, $Dir)
    $zipAsset = Find-Asset $Release "_$([regex]::Escape((Get-OSArch)))\.zip$"
    if (-not $zipAsset) { throw "Release $($Release.tag_name) has no portable $(Get-OSArch) zip." }
    $relVer = Get-AssetVersion -AssetName $zipAsset.name -TagName $Release.tag_name
    $null = Install-Portable -Release $Release -RelVer $relVer -Dir $Dir -Cache $Cache
    # Agentmaster.exe (current releases) or WindowsTerminal.exe (a pinned pre-rename -Version).
    $appExe = @('Agentmaster.exe', 'WindowsTerminal.exe') | ForEach-Object { Join-Path $Dir $_ } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $appExe) { $appExe = Join-Path $Dir 'Agentmaster.exe' }
    Write-Host ""
    Write-Host "Done." -ForegroundColor White
    Write-Info "Launch: `"$appExe`"  (or the 'Agentmaster (Portable)' Start-menu entry)"
    Write-Info "Upgrade: re-run this script (add -Portable to skip the install-mode question)."
    if ($Launch -and (Test-Path $appExe)) {
        Write-Step "Launching Agentmaster"
        Start-Process $appExe
    }
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
        if (-not $zipAsset) { throw "Release $tag has no portable $(Get-OSArch) zip." }
        Write-Ok "release $tag  (version $(Get-AssetVersion -AssetName $zipAsset.name -TagName $tag))"
        Invoke-PortableFlow -Release $release -Cache $DownloadDir -Dir $InstallDir
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

    # The .cer is tiny - fetched first, so the non-admin question is asked only when it actually
    # matters (this release's cert not yet machine-trusted) and BEFORE the big bundle download.
    $cerPath = Save-Asset $cerAsset $DownloadDir

    # Not admin + the cert needs machine-wide trust -> ask how to proceed (administrator install
    # preferred; user-level portable the safe fallback; cancel). -Elevate pre-answers admin, and
    # a non-interactive session keeps the classic straight-to-UAC behavior (never hangs).
    if (-not (Test-Admin) -and -not $Elevate -and (Test-CanPrompt) -and -not (Test-CertTrustedMachine $cerPath)) {
        if ((Invoke-InstallModeChoice -Release $release -Cache $DownloadDir -PortableDir $InstallDir) -ne 'admin') { return }
    }

    $bundlePath = Save-Asset $bundleAsset $DownloadDir

    # Trust the cert (may elevate). Interactively, a declined/failed elevation re-offers the
    # install-mode question instead of dying - the user can still land the no-admin fallback.
    while ($true) {
        try {
            Approve-SigningCert -CerPath $cerPath
            break
        } catch {
            if ((Test-Admin) -or $Elevate -or -not (Test-CanPrompt)) { throw }
            Write-Warn $_.Exception.Message
            if ((Invoke-InstallModeChoice -Release $release -Cache $DownloadDir -PortableDir $InstallDir) -ne 'admin') { return }
        }
    }

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
