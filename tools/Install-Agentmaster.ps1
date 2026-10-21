# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Install, upgrade or uninstall Agentmaster from its GitHub Releases - fully automated.

.DESCRIPTION
    Asks WHERE to install, and prefers the install that needs NO administrator rights:

      [1] Just for you (the default)
          %USERPROFILE%\.agentmaster\bin, no administrator rights. The same install the portable zip's own
          first launch makes: Start-menu and desktop shortcuts, an "Open in Agentmaster" folder right-click
          entry, an Apps & Features entry with Uninstall, the folder on your PATH. It updates itself in place.
      [2] For all users of this PC
          <Program Files>\Agentmaster, needs administrator rights (one UAC prompt). Every user keeps their own
          data; its updates need administrator approval too.
      [3] As the app package (MSIX)
          The self-signed package. Each release is signed with its own certificate, which must be trusted
          machine-wide: administrator approval once PER RELEASE.

    Re-running the script is the upgrade path: an existing install is found and upgraded in place, and the
    question's default follows what is installed. Only the files Agentmaster ships are replaced or removed;
    your data (sessions, settings, the profile folder) and anything else you keep in the folder stay.

    The script installs for the user SIGNED IN to Windows, even when it runs under another account (started
    with an administrator's UAC credentials, with "Run as different user", or by a deployment tool as
    SYSTEM): the per-user part then runs as the signed-in user (a one-shot scheduled task) and Agentmaster
    starts as that user, without administrator rights. -ForCurrentAccount installs for the running account
    instead.

    Safe to re-run any time.

.PARAMETER Scope
    User (just for you - no administrator rights), Machine (all users - administrator rights) or Package
    (the MSIX). Without it the script asks; a non-interactive run takes the default (User on a fresh PC,
    else the kind already installed).

.PARAMETER Version
    Specific version to install, e.g. "0.4.0" (a leading "v" is fine). Default: the latest release.
    A nightly is named by its FULL suffixed version (e.g. "0.6.10-prerelease-nightly").

.PARAMETER Prerelease
    Consider pre-releases when picking the newest version (ignored when -Version is given). NIGHTLY builds
    are NOT considered - they are a tier below pre-release (see -Nightly).

.PARAMETER Nightly
    Also consider NIGHTLY builds (unstable development builds; their tag contains "nightly"). Each switch
    admits only its own tier (-Nightly alone = nightlies + stable; -Prerelease alone = betas + stable).

.PARAMETER Portable
    The older name of -Scope User.

.PARAMETER Elevate
    The older name of -Scope Package, without asking how to trust the certificate (straight to UAC).

.PARAMETER ForCurrentAccount
    Install for the account running this script even when another user is signed in to Windows.

.PARAMETER Force
    Reinstall even if the installed version is the same or newer. For an all-users install it also closes
    other users' running instances.

.PARAMETER Launch
    Start Agentmaster after a successful install/upgrade (always without administrator rights).

.PARAMETER Uninstall
    Remove Agentmaster: the install of -Scope, or the one installed (it asks when there are several). Your
    profile data is never touched.

.PARAMETER InstallDir
    The install folder of a User or Machine install. Default: the existing install's folder, else
    %USERPROFILE%\.agentmaster\bin (User) or <Program Files>\Agentmaster (Machine).

.PARAMETER DownloadDir
    Where to cache downloads. Default: %TEMP%\Agentmaster-install.

.PARAMETER Repo
    owner/name of the GitHub repo. Default: Nucs/Agentmaster.

.PARAMETER GitHubToken
    Optional token to lift the unauthenticated GitHub API rate limit. Defaults to $env:GITHUB_TOKEN.

.EXAMPLE
    .\Install-Agentmaster.ps1
    Ask where to install (Enter = just for you, no administrator rights), then install or upgrade.

.EXAMPLE
    .\Install-Agentmaster.ps1 -Scope User -Launch
    Install or upgrade just for you without asking, and start it.

.EXAMPLE
    .\Install-Agentmaster.ps1 -Scope Machine
    Install or upgrade for all users of this PC (one UAC prompt).

.EXAMPLE
    .\Install-Agentmaster.ps1 -Uninstall
    Remove the installed Agentmaster (it asks which one when several kinds are installed).

.EXAMPLE
    # Run straight from the web (latest):
    irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1 | iex

    # ... or with arguments:
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Scope User -Launch

.NOTES
    Agentmaster is a fork of Windows Terminal. Keep this file pure ASCII (Windows PowerShell 5.1 reads a
    BOM-less script in the ANSI code page). The block between the SHARED INSTALL CORE markers is verbatim in
    src\cascadia\TerminalApp\AgentMaster\am-update.ps1 (tools\tests\Test-AgentmasterInstaller.ps1 checks).
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('', 'User', 'Machine', 'Package')]
    [string]   $Scope = '',
    [string]   $Version,
    [switch]   $Prerelease,
    [switch]   $Nightly,
    [switch]   $Portable,
    [switch]   $Elevate,
    [switch]   $ForCurrentAccount,
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

# ---- script-only helpers --------------------------------------------------------------------------------
<#
.SYNOPSIS
    Refuse anything but Windows.
.NOTES
    Throws on another OS.
#>
function Test-Windows {
    if ($env:OS -ne 'Windows_NT') { throw "Agentmaster runs on Windows only." }
}

<#
.SYNOPSIS
    Refuse a 32-bit PowerShell on 64-bit Windows: its registry and Program Files views are redirected
    (WOW64), so an all-users install would register in the wrong place.
.NOTES
    Throws in a 32-bit host on a 64-bit OS.
#>
function Test-64BitHost {
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        throw "This is the 32-bit PowerShell. Run the installer from the normal (64-bit) Windows PowerShell or PowerShell 7."
    }
}

<#
.SYNOPSIS
    True when a human can answer questions here: an interactive session whose host was not started
    -NonInteractive ('-non' is the shortest prefix PowerShell itself accepts, so any spelling matches), and
    whose console input is the keyboard.
.DESCRIPTION
    The console host reads Read-Host from standard input when that is redirected (a file, NUL, a pipe from a
    CI runner or an automation agent): the question would be "answered" at once with an empty line, so it
    is not asked at all and the run says which default it took. Other hosts (the ISE, an editor's console)
    prompt through their own UI and are left to Read-Host.
.OUTPUTS
    [bool]
#>
function Test-CanPrompt {
    if (-not [Environment]::UserInteractive) { return $false }
    if ([Environment]::GetCommandLineArgs() -match '(?i)^[-/]non') { return $false }
    if ($Host.Name -eq 'ConsoleHost') {
        $redirected = $false
        # A host without a console handle can make the probe throw: then nothing says the input is not the
        # keyboard, and Read-Host is tried as before.
        try { $redirected = [Console]::IsInputRedirected } catch { $redirected = $false }
        if ($redirected) { return $false }
    }
    return $true
}

<#
.SYNOPSIS
    Ask one console question with single-key answers.
.PARAMETER Prompt
    The question line.
.PARAMETER Answers
    A hashtable: accepted answer (lower-case) -> the value returned.
.PARAMETER Default
    The value returned for Enter (and when the host cannot prompt after all).
.OUTPUTS
    The mapped value.
#>
function Read-Answer {
    param([string]$Prompt, [hashtable]$Answers, $Default)
    while ($true) {
        try { $ans = Read-Host $Prompt } catch { return $Default }
        $a = ("$ans").Trim().ToLowerInvariant()
        if (-not $a) { return $Default }
        if ($Answers.ContainsKey($a)) { return $Answers[$a] }
        Write-Warn "unrecognized answer '$ans'"
    }
}

# ---- GitHub release resolution ---------------------------------------------------------------------------
<#
.SYNOPSIS
    The GitHub API request headers (a token lifts the unauthenticated rate limit).
.OUTPUTS
    [hashtable]
#>
function Get-GitHubHeaders {
    $h = @{ 'User-Agent' = 'Agentmaster-Installer'; 'Accept' = 'application/vnd.github+json' }
    if ($GitHubToken) { $h['Authorization'] = "Bearer $GitHubToken" }
    return $h
}

<#
.SYNOPSIS
    True for a NIGHTLY release tag (it contains "nightly") - the in-app updater's contract (IsNightlyTag).
.PARAMETER TagName
    The release tag.
.OUTPUTS
    [bool]
#>
function Test-NightlyTag {
    param($TagName)
    return ("$TagName" -match '(?i)nightly')
}

<#
.SYNOPSIS
    The release to install: -Version, or the newest on the requested channel.
.DESCRIPTION
    The in-app updater's channel model (Updater.h ReleaseAllowedOnChannel): stable is always eligible; a
    NIGHTLY (by tag) only under -Nightly; any other prerelease only under -Prerelease.
.PARAMETER Headers
    The API headers.
.OUTPUTS
    The GitHub release object.
.NOTES
    Throws when the release does not exist or nothing is on the channel.
#>
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
    if ($Prerelease -or $Nightly) {
        $incl = if ($Prerelease -and $Nightly) { 'pre-releases + nightlies' }
                elseif ($Nightly)              { 'nightlies' }
                else                           { 'pre-releases' }
        Write-Step "Resolving newest release (incl. $incl) from $Repo"
        $all = Invoke-RestMethod -Uri "$base`?per_page=20" -Headers $Headers
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
    try {
        $r = Invoke-RestMethod -Uri "$base/latest" -Headers $Headers
    } catch {
        # GitHub answers /latest with 404 while a repository has only pre-releases (and no stable one yet):
        # say what to do instead of showing the raw web error. Any other failure (network, rate limit) is
        # shown as it is.
        $status = 0
        try { $status = [int]$_.Exception.Response.StatusCode } catch { $status = 0 }
        if ($status -eq 404) { throw "$Repo has no stable release yet. Install the newest pre-release with -Prerelease, or a given one with -Version <x.y.z>." }
        throw
    }
    if ($r -and (Test-NightlyTag $r.tag_name)) {
        Write-Warn "latest ($($r.tag_name)) is a NIGHTLY (unstable dev build) - skipped on the default channel (use -Nightly to opt in)"
        $all = Invoke-RestMethod -Uri "$base`?per_page=20" -Headers $Headers
        $r = $all | Where-Object { -not $_.draft -and -not $_.prerelease -and -not (Test-NightlyTag $_.tag_name) } | Select-Object -First 1
        if (-not $r) { throw "No stable (non-nightly) release found in $Repo. Use -Version <x.y.z>, -Prerelease, or -Nightly." }
    }
    return $r
}

<#
.SYNOPSIS
    The 4-part version of an asset (from its name, else from the tag).
.PARAMETER AssetName
    The asset's file name; a "_1.2.3.4" part in it wins (the MSIX bundle names carry the full version).
.PARAMETER TagName
    The release tag ("v1.2.3", "v1.2.3-prerelease-nightly"), used when the name carries no version: its
    first dash-separated part gets a ".0" revision.
.OUTPUTS
    [version]
.NOTES
    Throws (a [version] cast) for a tag whose first part is not a dotted number.
#>
function Get-AssetVersion {
    param($AssetName, $TagName)
    if ($AssetName -match '_(\d+\.\d+\.\d+\.\d+)') { return [version]$Matches[1] }
    return [version]("{0}.0" -f (($TagName -replace '^v', '') -split '-')[0])
}

<#
.SYNOPSIS
    True when a downloaded file matches its asset's sha256 digest (or the asset carries none).
.PARAMETER Path
    The downloaded file.
.PARAMETER Asset
    The release asset object from the GitHub API; its optional "digest" property reads "sha256:<hex>".
    GitHub added the property in 2025, so an asset without one (or with another algorithm) passes.
.OUTPUTS
    [bool]
.NOTES
    Throws only when the file cannot be read for hashing.
#>
function Test-Digest {
    param($Path, $Asset)
    $digest = $null
    if ($Asset.PSObject.Properties.Name -contains 'digest') { $digest = $Asset.digest }
    if (-not $digest) { return $true }
    if ($digest -match '^sha256:(?<h>[0-9a-fA-F]{64})$') {
        $have = Get-FileSha256 $Path
        return ($have -ieq $Matches.h)
    }
    return $true
}

<#
.SYNOPSIS
    Download a release asset into $Dir (cached and SHA-256 verified).
.PARAMETER Asset
    The release asset object from the GitHub API (name, size, browser_download_url, optional digest).
.PARAMETER Dir
    The download folder. A file of the asset's name, size and digest already there is reused.
.OUTPUTS
    [string] the local path.
.NOTES
    Throws on a download failure or a checksum mismatch (the corrupt file stays, and the next run downloads
    it again because its digest does not match).
#>
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

<#
.SYNOPSIS
    The first asset of a release whose name matches a pattern.
.PARAMETER Release
    The release object from the GitHub API.
.PARAMETER Pattern
    A regular expression for the asset name, or a wildcard pattern with -Like.
.PARAMETER Like
    Match $Pattern as a wildcard (-like) instead of a regular expression.
.OUTPUTS
    The asset object, or $null.
.NOTES
    Never throws for a valid pattern; an invalid regular expression throws.
#>
function Find-Asset {
    param($Release, [string]$Pattern, [switch]$Like)
    $a = $Release.assets | Where-Object { if ($Like) { $_.name -like $Pattern } else { $_.name -match $Pattern } } | Select-Object -First 1
    return $a
}

# ---- what is installed ------------------------------------------------------------------------------------
<#
.SYNOPSIS
    What is installed for the user this run installs for (the signed-in user, unless -ForCurrentAccount):
    the per-user install, the all-users install, the MSIX package.
.DESCRIPTION
    When another account runs the script, the signed-in user's per-user install is read through their
    loaded registry hive (HKEY_USERS\<sid>) and their profile folder, and their package through
    Get-AppxPackage -User (administrator rights).
.PARAMETER Identity
    Get-AmIdentity's result.
.OUTPUTS
    [pscustomobject] UserDir, UserVersion, MachineDir, MachineVersion, Package, PackageRunning.
#>
function Get-InstallState {
    param($Identity)
    $s = [ordered]@{ UserDir = ''; UserVersion = $null; MachineDir = ''; MachineVersion = $null; Package = $null; PackageRunning = $false }
    if ($Identity.IsMainUser -or $ForCurrentAccount) {
        $s.UserDir = Find-AmUserInstallDir
        $s.Package = Get-AmPackage -FamilyName $FamilyName
    } elseif ($Identity.MainUserSid) {
        $loc = ''
        $key = "Registry::HKEY_USERS\$($Identity.MainUserSid)\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster"
        # Test-Path first (no thrown "not found" for the usual case: nothing installed yet); the catch is for
        # a hive that exists but cannot be read.
        if (Test-Path -LiteralPath $key) {
            try { $loc = [string](Get-ItemProperty -LiteralPath $key -ErrorAction Stop).InstallLocation } catch { $loc = '' }
        }
        if ($loc -and (Get-AppExe $loc)) {
            $s.UserDir = $loc
        } elseif ($Identity.MainUserProfile) {
            $legacy = Join-Path $Identity.MainUserProfile 'AppData\Local\Programs\Agentmaster'
            if (Get-AppExe $legacy) { $s.UserDir = $legacy }
        }
        if ($Identity.Elevated -or $Identity.IsSystem) {
            try { $s.Package = Get-AppxPackage -User $Identity.MainUserSid -ErrorAction Stop | Where-Object { $_.PackageFamilyName -eq $FamilyName } | Select-Object -First 1 } catch { $s.Package = $null }
        }
    }
    if ($s.UserDir) { $s.UserVersion = Get-AmInstalledVersion -Dir $s.UserDir }
    $s.MachineDir = Find-AmMachineInstallDir
    if ($s.MachineDir) { $s.MachineVersion = Get-AmInstalledVersion -Dir $s.MachineDir }
    if ($s.Package -and $s.Package.InstallLocation) {
        $loc = $s.Package.InstallLocation.TrimEnd('\') + '\'
        $s.PackageRunning = [bool](Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($loc, [StringComparison]::OrdinalIgnoreCase) })
    }
    return [pscustomobject]$s
}

<#
.SYNOPSIS
    Say for whom this run installs, when that is not simply the account running it.
.PARAMETER Identity
    Get-AmIdentity's result.
#>
function Show-IdentityNote {
    param($Identity)
    if (-not $Identity.IsMainUser) {
        if ($ForCurrentAccount) {
            Write-Warn "Running as $($Identity.RunningName) while $($Identity.MainUserName) is signed in: installing for $($Identity.RunningName) (-ForCurrentAccount)."
        } elseif ($Identity.Elevated -or $Identity.IsSystem) {
            Write-Warn "Running as $($Identity.RunningName), but $($Identity.MainUserName) is signed in to Windows: installing for $($Identity.MainUserName)."
            Write-Info "(-ForCurrentAccount installs for $($Identity.RunningName) instead)"
        } else {
            # A standard account cannot schedule anything for another user, so a per-user install for the
            # signed-in user is impossible from here: say so before anything is downloaded (the per-user flow
            # then stops; an install for all users still works, through the usual approval).
            Write-Warn "Running as $($Identity.RunningName), but $($Identity.MainUserName) is signed in to Windows, and this account cannot install for $($Identity.MainUserName)."
            Write-Info "Run the installer from $($Identity.MainUserName)'s own PowerShell, or add -ForCurrentAccount to install for $($Identity.RunningName)."
        }
    } elseif ($Identity.Elevated) {
        Write-Info 'Running with administrator rights. A per-user install needs none, and Agentmaster starts without them.'
    }
}

<#
.SYNOPSIS
    The scope question: where Agentmaster should be installed. The no-administrator per-user install is the
    default on a fresh PC; when something is installed already, the default is to upgrade THAT.
.PARAMETER Identity
    Get-AmIdentity's result.
.PARAMETER State
    Get-InstallState's result.
.OUTPUTS
    [string] User, Machine, Package or Cancel.
#>
function Read-InstallScope {
    param($Identity, $State)
    $default = 'User'
    if (-not $State.UserDir) {
        if ($State.MachineDir) { $default = 'Machine' }
        elseif ($State.Package) { $default = 'Package' }
    }
    if (-not (Test-CanPrompt)) {
        Write-Info "non-interactive: installing for scope $default (pass -Scope to choose)"
        return $default
    }
    $forMain = (-not $Identity.IsMainUser) -and -not $ForCurrentAccount
    $who = if ($forMain) { "for $($Identity.MainUserName)" } else { 'just for you' }
    $userDir = if ($InstallDir) { $InstallDir }
               elseif ($State.UserDir) { "$($State.UserDir)  (installed: v$($State.UserVersion) - this upgrades it)" }
               elseif ($forMain -and $Identity.MainUserProfile) { Join-Path $Identity.MainUserProfile '.agentmaster\bin' }
               else { Get-AmDefaultUserInstallDir }
    $machineDir = if ($State.MachineDir) { "$($State.MachineDir)  (installed: v$($State.MachineVersion) - this upgrades it)" } else { Get-AmDefaultMachineInstallDir }
    $pkgNote = if ($State.Package) { "  (installed: v$($State.Package.Version) - this upgrades it)" } else { '' }
    $keys = @{ 'User' = '1'; 'Machine' = '2'; 'Package' = '3' }
    Write-Host ''
    Write-Host '    Where should Agentmaster be installed?' -ForegroundColor White
    Write-Host ''
    # The label starts the line: "Just for you" / "For DOMAIN\user" (the sentence form stays lower-case).
    $whoLabel = $who.Substring(0, 1).ToUpperInvariant() + $who.Substring(1)
    Write-Host "      [1] $whoLabel (recommended - no administrator rights)" -ForegroundColor White
    Write-Host "          $userDir"
    Write-Host '      [2] For all users of this PC (needs administrator rights)' -ForegroundColor White
    Write-Host "          $machineDir"
    Write-Host '      [3] As the app package (MSIX; needs administrator approval for each release''s certificate)' -ForegroundColor White
    if ($pkgNote) { Write-Host "         $pkgNote" }
    Write-Host '      [C] Cancel' -ForegroundColor White
    Write-Host ''
    return (Read-Answer -Prompt "    Choice [1/2/3/C] (Enter = $($keys[$default]))" -Default $default -Answers @{
        '1' = 'User'; 'u' = 'User'; 'user' = 'User'
        '2' = 'Machine'; 'm' = 'Machine'; 'machine' = 'Machine'; 'a' = 'Machine'
        '3' = 'Package'; 'p' = 'Package'; 'package' = 'Package'; 'msix' = 'Package'
        'c' = 'Cancel'; 'cancel' = 'Cancel'; 'q' = 'Cancel'; 'n' = 'Cancel'
    })
}

<#
.SYNOPSIS
    Whether to remove the MSIX package after a per-user / all-users install (the takeover: both use the
    same data). Asked when interactive; default yes unless the package is running.
.PARAMETER State
    Get-InstallState's result.
.OUTPUTS
    [bool]
#>
function Read-PackageTakeover {
    param($State)
    if (-not $State.Package) { return $false }
    if (-not (Test-CanPrompt)) {
        Write-Info "the installed package v$($State.Package.Version) is left in place (non-interactive)"
        return $false
    }
    $def = -not $State.PackageRunning
    $hint = if ($def) { '[Y/n]' } else { '[y/N] (it is running - removing it closes its windows)' }
    return [bool](Read-Answer -Prompt "    The Agentmaster package v$($State.Package.Version) is installed too. Remove it after this install? Your data is kept $hint" `
        -Default $def -Answers @{ 'y' = $true; 'yes' = $true; 'n' = $false; 'no' = $false })
}

# ---- the three install kinds -----------------------------------------------------------------------------
<#
.SYNOPSIS
    Install or upgrade the per-user copy (no administrator rights) for the signed-in user.
.PARAMETER Release
    The GitHub release.
.PARAMETER Identity
    Get-AmIdentity's result.
.PARAMETER State
    Get-InstallState's result.
.NOTES
    Throws when the release has no portable zip for this OS, when another account runs the script without
    the rights to act for the signed-in user, or when the install fails.
#>
function Invoke-UserScopeFlow {
    param($Release, $Identity, $State)
    $forMain = (-not $Identity.IsMainUser) -and -not $ForCurrentAccount
    # Refuse BEFORE the download: a standard account cannot schedule the per-user part for another user.
    if ($forMain -and -not ($Identity.Elevated -or $Identity.IsSystem)) {
        throw "This PowerShell runs as $($Identity.RunningName), but $($Identity.MainUserName) is signed in. Run the installer from $($Identity.MainUserName)'s own PowerShell (no 'Run as administrator' needed), or add -ForCurrentAccount to install for $($Identity.RunningName)."
    }
    $zipAsset = Find-Asset $Release "_$([regex]::Escape((Get-OSArch)))\.zip$"
    if (-not $zipAsset) { throw "Release $($Release.tag_name) has no portable $(Get-OSArch) zip." }
    $relVer = Get-AssetVersion -AssetName $zipAsset.name -TagName $Release.tag_name
    Write-Ok "release $($Release.tag_name)  (version $relVer)"
    $removePackage = Read-PackageTakeover -State $State
    $zipPath = Save-Asset $zipAsset $DownloadDir
    if (-not $forMain) {
        $exe = Invoke-AmUserInstall -ZipPath $zipPath -Version "$relVer" -DestDir $InstallDir -Force:$Force -RemovePackage:$removePackage
    } else {
        $q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
        $body = "`$null = Invoke-AmUserInstall -ZipPath (Join-Path `$PhaseDir " + (& $q $zipAsset.name) + ") -Version " + (& $q "$relVer") +
                " -DestDir " + (& $q $InstallDir) + $(if ($Force) { ' -Force' } else { '' }) + $(if ($removePackage) { ' -RemovePackage' } else { '' }) +
                " -ResultFile (Join-Path `$PhaseDir 'result.txt')"
        $r = Invoke-AmPhaseAsUser -Identity $Identity -Body $body -Files @($zipPath)
        if ($r.Code -ne 0) { throw "The install for $($Identity.MainUserName) did not complete (exit code $($r.Code)); its record is above." }
        $exe = $r.Result
    }
    Write-Host ''
    Write-Host ("Done - Agentmaster $relVer is installed " + $(if ($forMain) { "for $($Identity.MainUserName)" } else { 'just for you' }) + ': ' + $exe) -ForegroundColor White
    Write-Info "Launch: the 'Agentmaster' Start-menu entry, the desktop icon, or 'Agentmaster.exe' on your PATH."
    Write-Info 'Upgrade: Agentmaster updates itself; re-running this script works too.'
    if ($State.MachineDir) { Write-Info "Agentmaster is also installed for all users at $($State.MachineDir): Start shows both." }
    if ($Launch -and $exe) {
        Write-Step 'Launching Agentmaster'
        $null = Start-AmAppAsUser -Exe $exe -Identity $Identity
    }
}

<#
.SYNOPSIS
    Install or upgrade the all-users copy (administrator rights: one UAC prompt when this PowerShell is not
    elevated; only that step runs elevated).
.PARAMETER Release
    The GitHub release.
.PARAMETER Identity
    Get-AmIdentity's result.
.PARAMETER State
    Get-InstallState's result.
.NOTES
    Throws when the approval is declined or the install fails.
#>
function Invoke-MachineScopeFlow {
    param($Release, $Identity, $State)
    $zipAsset = Find-Asset $Release "_$([regex]::Escape((Get-OSArch)))\.zip$"
    if (-not $zipAsset) { throw "Release $($Release.tag_name) has no portable $(Get-OSArch) zip." }
    $relVer = Get-AssetVersion -AssetName $zipAsset.name -TagName $Release.tag_name
    Write-Ok "release $($Release.tag_name)  (version $relVer)"
    $zipPath = Save-Asset $zipAsset $DownloadDir
    $dest = if ($InstallDir) { $InstallDir } elseif ($State.MachineDir) { $State.MachineDir } else { Get-AmDefaultMachineInstallDir }
    if (Test-Admin) {
        $exe = Invoke-AmMachineInstall -ZipPath $zipPath -Version "$relVer" -DestDir $dest -Force:$Force
    } else {
        Write-Step 'Installing for all users needs administrator rights - Windows asks for approval'
        $q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
        $body = "`$null = Invoke-AmMachineInstall -ZipPath (Join-Path `$PhaseDir " + (& $q $zipAsset.name) + ") -Version " + (& $q "$relVer") +
                " -DestDir " + (& $q $dest) + $(if ($Force) { ' -Force' } else { '' }) + " -ResultFile (Join-Path `$PhaseDir 'result.txt')"
        $r = Invoke-AmElevatedPhase -Body $body -Files @($zipPath)
        if ($r.Code -eq 1223) { throw 'Administrator approval was declined - nothing was installed. Choose "Just for you" (-Scope User) for an install without administrator rights.' }
        if ($r.Code -eq 1260) { throw 'Windows does not let this account run programs as administrator (a setting of this PC), so it cannot install for all users - nothing was installed. Choose "Just for you" (-Scope User): it needs no administrator rights.' }
        if ($r.Code -ne 0) { throw "The install for all users did not complete (exit code $($r.Code)); its record is above." }
        $exe = $r.Result
    }
    Write-Host ''
    Write-Host "Done - Agentmaster $relVer is installed for all users: $exe" -ForegroundColor White
    Write-Info 'Every user keeps their own data; updating it needs administrator approval.'
    if ($State.UserDir) { Write-Info "A per-user copy is installed too, at $($State.UserDir): Start shows both (remove it with -Uninstall -Scope User)." }
    if ($Launch -and $exe) {
        Write-Step 'Launching Agentmaster'
        $null = Start-AmAppAsUser -Exe $exe -Identity $Identity
    }
}

<#
.SYNOPSIS
    Missing framework dependencies of a failed Add-AppxPackage: VCLibs is fetched; Microsoft.UI.Xaml cannot
    be fetched reliably (the per-user install needs neither).
.PARAMETER Message
    The failed Add-AppxPackage's exception message, which names the missing framework.
.PARAMETER Dir
    The download cache the VCLibs package goes to.
.OUTPUTS
    [string[]] dependency package paths (empty when nothing could be fetched).
.NOTES
    Never throws: a failed VCLibs download is a warning, and the caller then rethrows the original error.
#>
function Resolve-MissingDeps {
    param($Message, $Dir)
    $deps = @()
    if ($Message -match 'VCLibs') {
        try { $deps += Get-VCLibs $Dir } catch { Write-Warn "could not fetch VCLibs: $($_.Exception.Message)" }
    }
    if ($Message -match 'Microsoft\.UI\.Xaml') {
        Write-Warn "This build needs the 'Microsoft.UI.Xaml' framework, which can't be auto-fetched reliably."
        Write-Warn "Install it from the Microsoft Store, or install just for you instead: re-run with -Scope User."
    }
    return , $deps
}

<#
.SYNOPSIS
    A conflicting existing registration (a registered loose-layout dev install, or any same-identity install
    that cannot be replaced in place) makes Add-AppxPackage fail with 0x80073CFB: offer to remove it.
.DESCRIPTION
    Per-user removal needs no administrator rights, and Agentmaster's data lives OUTSIDE the package, so it
    survives. (am-update.ps1's twin deliberately never asks: the user clicked "Update now" in the app.)
.PARAMETER AutoYes
    Remove without asking (-Force).
.OUTPUTS
    [bool] true when the blocker was removed.
#>
function Remove-BlockingInstall {
    param([bool]$AutoYes)
    $pkg = Get-AmPackage -FamilyName $FamilyName
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

<#
.SYNOPSIS
    Add-AppxPackage the bundle, resolving a missing VCLibs dependency or a blocking install once.
.PARAMETER BundlePath
    The downloaded .msixbundle.
.PARAMETER Dir
    The download cache (for VCLibs).
.PARAMETER ForceFlag
    Force-apply over any version and close a running instance.
.NOTES
    Throws when the install fails for another reason. (am-update.ps1's twin always force-applies.)
#>
function Install-Bundle {
    param($BundlePath, $Dir, [bool]$ForceFlag)
    $splat = @{ Path = $BundlePath; ErrorAction = 'Stop' }
    if ($ForceFlag) { $splat['ForceUpdateFromAnyVersion'] = $true; $splat['ForceApplicationShutdown'] = $true }
    try {
        Add-AppxPackage @splat
        return
    } catch {
        $msg = $_.Exception.Message
        if ($msg -match '0x80073CF3' -or $msg -match 'dependency') {
            Write-Warn "Resolving missing framework dependencies..."
            $deps = Resolve-MissingDeps -Message $msg -Dir $Dir
            if ($deps -and $deps.Count -gt 0) {
                $splat['DependencyPath'] = $deps
                Add-AppxPackage @splat
                return
            }
        }
        if ($msg -match '0x80073CFB' -or $msg -match 'already installed' -or
            $msg -match 'cannot replace' -or $msg -match 'unpackaged') {
            if (Remove-BlockingInstall -AutoYes:$ForceFlag) {
                Add-AppxPackage @splat
                return
            }
        }
        throw
    }
}

<#
.SYNOPSIS
    Install or upgrade the MSIX package for the signed-in user. Its certificate needs administrator approval
    once per release; when that is not available, the per-user install is offered instead.
.PARAMETER Release
    The GitHub release.
.PARAMETER Identity
    Get-AmIdentity's result.
.PARAMETER State
    Get-InstallState's result.
.NOTES
    Throws when the release lacks the bundle or the certificate, or the install fails.
#>
function Invoke-PackageScopeFlow {
    param($Release, $Identity, $State)
    $bundleAsset = Find-Asset $Release '*.msixbundle' -Like
    if (-not $bundleAsset) { throw "Release $($Release.tag_name) has no .msixbundle asset." }
    $cerAsset = Find-Asset $Release '*.cer' -Like
    if (-not $cerAsset) { throw "Release $($Release.tag_name) has no .cer (signing certificate) asset." }
    $relVer = Get-AssetVersion -AssetName $bundleAsset.name -TagName $Release.tag_name
    Write-Ok "release $($Release.tag_name)  (version $relVer)"
    $forMain = (-not $Identity.IsMainUser) -and -not $ForCurrentAccount
    if ($forMain -and -not ($Identity.Elevated -or $Identity.IsSystem)) {
        throw "This PowerShell runs as $($Identity.RunningName), but $($Identity.MainUserName) is signed in. Run the installer from $($Identity.MainUserName)'s own PowerShell, or add -ForCurrentAccount."
    }
    $installed = $State.Package
    if ($installed) {
        $iv = [version]$installed.Version
        if (($iv -ge $relVer) -and -not $Force) {
            Write-Ok "Already up to date (installed $iv >= release $relVer). Use -Force to reinstall."
            if ($Launch) { Start-AmPackageApp -Identity $Identity }
            return
        }
        Write-Step "Upgrading $iv -> $relVer"
    } else {
        Write-Step "Installing $relVer"
    }
    # The .cer is tiny - fetched first, so the question is asked only when it matters (this release's
    # certificate not yet trusted) and BEFORE the big bundle download.
    $cerPath = Save-Asset $cerAsset $DownloadDir
    if (-not (Test-Admin) -and -not $Elevate -and (Test-CanPrompt) -and -not (Test-CertTrustedMachine $cerPath)) {
        if ((Read-PackageCertChoice) -eq 'user') { Invoke-UserScopeFlow -Release $Release -Identity $Identity -State $State; return }
    }
    # The certificate is trusted BEFORE the big bundle download: a declined or refused approval then costs
    # nothing (a non-interactive standard user downloaded the whole bundle first - test desktop, 2026-10-21).
    while ($true) {
        try {
            Approve-SigningCert -CerPath $cerPath
            break
        } catch {
            if ((Test-Admin) -or $Elevate -or -not (Test-CanPrompt)) { throw }
            Write-Warn $_.Exception.Message
            if ((Read-PackageCertChoice) -eq 'user') { Invoke-UserScopeFlow -Release $Release -Identity $Identity -State $State; return }
        }
    }
    $bundlePath = Save-Asset $bundleAsset $DownloadDir
    Write-Step "Installing the package"
    if ($forMain) {
        # Add-AppxPackage registers for the account that runs it: the signed-in user's own task does it.
        $q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
        $body = "`$b = Join-Path `$PhaseDir " + (& $q $bundleAsset.name) + "`r`n" +
                "try { Add-AppxPackage -Path `$b -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop } catch {`r`n" +
                "  if (`$_.Exception.Message -match '0x80073CF3|VCLibs|dependency') { Add-AppxPackage -Path `$b -DependencyPath (Get-VCLibs `$PhaseDir) -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop } else { throw } }`r`n" +
                "Write-Ok 'package added'"
        $r = Invoke-AmPhaseAsUser -Identity $Identity -Body $body -Files @($bundlePath)
        if ($r.Code -ne 0) { throw "The package install for $($Identity.MainUserName) did not complete (exit code $($r.Code)); its record is above." }
    } else {
        Install-Bundle -BundlePath $bundlePath -Dir $DownloadDir -ForceFlag ([bool]$Force)
        $now = Get-AmPackage -FamilyName $FamilyName
        if (-not $now) { throw "Install reported success but the package isn't registered. See the error above." }
    }
    Write-Host ""
    Write-Host "Done - Agentmaster $relVer is installed as the app package." -ForegroundColor White
    Write-Info "Launch: run 'agentmaster' or use the 'Agentmaster' Start-menu entry."
    if ($installed -and -not $Force) { Write-Info "If Agentmaster is currently running, restart it to pick up the new version." }
    if ($State.UserDir -or $State.MachineDir) { Write-Info 'A copy installed from the portable zip exists too: Start shows both, and they share your data.' }
    if ($Launch) { Start-AmPackageApp -Identity $Identity }
}

<#
.SYNOPSIS
    The certificate question of a package install without administrator rights: approve (UAC), install just
    for you instead (no administrator rights), or cancel.
.OUTPUTS
    [string] admin or user; throws for cancel.
#>
function Read-PackageCertChoice {
    $canElevate = Test-CanElevate
    Write-Host ""
    Write-Host "    This release's package is signed with its own certificate, which must be trusted for the" -ForegroundColor Yellow
    Write-Host "    whole PC once - that needs administrator approval." -ForegroundColor Yellow
    if (-not $canElevate) { Write-Host "    Your account is not an administrator: Windows will ask for an administrator's password." -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "      [A] Approve as administrator (UAC), then install the package"
    Write-Host "      [U] Install just for you instead - no administrator rights, no certificate"
    Write-Host "      [C] Cancel"
    Write-Host ""
    $def = if ($canElevate) { 'admin' } else { 'user' }
    $pick = Read-Answer -Prompt ("    Choice [A/U/C] (Enter = " + $(if ($canElevate) { 'A' } else { 'U' }) + ")") -Default $def -Answers @{
        'a' = 'admin'; 'admin' = 'admin'; 'e' = 'admin'; 'u' = 'user'; 'user' = 'user'; 'p' = 'user'; 'c' = 'cancel'; 'cancel' = 'cancel'; 'n' = 'cancel' }
    if ($pick -eq 'cancel') { throw 'Cancelled - nothing was installed.' }
    return $pick
}

<#
.SYNOPSIS
    Start the packaged app for the signed-in user, without administrator rights.
.PARAMETER Identity
    Get-AmIdentity's result.
#>
function Start-AmPackageApp {
    param($Identity)
    Write-Step 'Launching Agentmaster'
    if ($Identity.IsMainUser -and -not $Identity.Elevated) { Start-Process "shell:appsFolder\$Aumid"; return }
    try {
        $null = Invoke-AmAsUser -UserSid $(if ($Identity.MainUserSid) { $Identity.MainUserSid } else { $Identity.RunningSid }) `
                    -FilePath "$env:SystemRoot\explorer.exe" -Arguments "shell:appsFolder\$Aumid" -Wait -TimeoutSec 60
    } catch { Write-Warn "start it from the Start menu: $($_.Exception.Message)" }
}

# ---- uninstall -------------------------------------------------------------------------------------------
<#
.SYNOPSIS
    Remove what is installed: the kind of -Scope, else the one kind installed (asks when there are several).
    Only Agentmaster's own files and registrations go; every profile folder and the exe-side user state
    (settings\, profile\, profile.path) stay.
.PARAMETER Identity
    Get-AmIdentity's result.
.PARAMETER State
    Get-InstallState's result.
.NOTES
    Throws when a removal needs rights this run does not have (the message says how), or several kinds are
    installed in a non-interactive run without -Scope.
#>
function Invoke-UninstallFlow {
    param($Identity, $State)
    $kinds = @()
    if ($State.UserDir) { $kinds += 'User' }
    if ($State.MachineDir) { $kinds += 'Machine' }
    if ($State.Package) { $kinds += 'Package' }
    $pick = @()
    if ($Scope) {
        $pick = @($Scope)
    } elseif ($kinds.Count -eq 0) {
        Write-Warn 'Agentmaster is not installed (no per-user install, no install for all users, no package).'
        return
    } elseif ($kinds.Count -eq 1) {
        $pick = $kinds
    } elseif (Test-CanPrompt) {
        Write-Host ''
        Write-Host '    Several Agentmaster installs exist. Remove which?' -ForegroundColor White
        if ($State.UserDir) { Write-Host "      [U] Just for you:   $($State.UserDir)" }
        if ($State.MachineDir) { Write-Host "      [M] For all users:  $($State.MachineDir)" }
        if ($State.Package) { Write-Host "      [P] The package:    $($State.Package.PackageFullName)" }
        Write-Host '      [A] All of them'
        Write-Host '      [C] Cancel'
        $answer = Read-Answer -Prompt '    Choice' -Default 'cancel' -Answers @{ 'u' = 'User'; 'm' = 'Machine'; 'p' = 'Package'; 'a' = 'all'; 'c' = 'cancel' }
        if ($answer -eq 'cancel') { Write-Warn 'Cancelled - nothing was removed.'; return }
        $pick = if ($answer -eq 'all') { $kinds } else { @($answer) }
    } else {
        throw ("Several Agentmaster installs exist (" + ($kinds -join ', ') + "): pass -Scope to say which to remove.")
    }
    $forMain = (-not $Identity.IsMainUser) -and -not $ForCurrentAccount
    $q = { param($s) "'" + ([string]$s).Replace("'", "''") + "'" }
    foreach ($kind in $pick) {
        switch ($kind) {
            'User' {
                if (-not $State.UserDir) { Write-Warn 'No per-user install found.'; continue }
                if (-not $forMain) {
                    $null = Uninstall-AmCopy -Scope User -Dir $State.UserDir
                } elseif ($Identity.Elevated -or $Identity.IsSystem) {
                    $r = Invoke-AmPhaseAsUser -Identity $Identity -Body ("`$null = Uninstall-AmCopy -Scope User -Dir " + (& $q $State.UserDir))
                    if ($r.Code -ne 0) { throw "Removing the per-user install of $($Identity.MainUserName) failed (exit code $($r.Code))." }
                } else {
                    throw "The per-user install belongs to $($Identity.MainUserName): run the uninstall from their own PowerShell."
                }
            }
            'Machine' {
                if (-not $State.MachineDir) { Write-Warn 'No install for all users found.'; continue }
                if (Test-Admin) {
                    $null = Uninstall-AmCopy -Scope Machine -Dir $State.MachineDir -Force:$Force
                } else {
                    Write-Step 'Removing the install for all users needs administrator rights - Windows asks for approval'
                    $r = Invoke-AmElevatedPhase -Body ("`$null = Uninstall-AmCopy -Scope Machine -Dir " + (& $q $State.MachineDir) + $(if ($Force) { ' -Force' } else { '' }))
                    if ($r.Code -eq 1223) { throw 'Administrator approval was declined - the install for all users was left in place.' }
                    if ($r.Code -eq 1260) { throw 'Windows does not let this account run programs as administrator (a setting of this PC): ask an administrator to remove the install for all users.' }
                    if ($r.Code -ne 0) { throw "Removing the install for all users failed (exit code $($r.Code))." }
                }
            }
            'Package' {
                if (-not $State.Package) { Write-Warn 'No package installed.'; continue }
                if (-not $forMain) {
                    Remove-AmPackage -Package $State.Package
                } elseif ($Identity.Elevated -or $Identity.IsSystem) {
                    $r = Invoke-AmPhaseAsUser -Identity $Identity -Body ("Remove-AmPackage -Package (Get-AmPackage -FamilyName " + (& $q $FamilyName) + ")")
                    if ($r.Code -ne 0) { throw "Removing the package of $($Identity.MainUserName) failed (exit code $($r.Code))." }
                } else {
                    throw "The package belongs to $($Identity.MainUserName): run the uninstall from their own PowerShell."
                }
            }
        }
    }
    Write-Info 'Your profile data (sessions, settings, window layouts - e.g. %USERPROFILE%\.agentmaster) was left untouched.'
}

# ============================ main ============================================================
# Tests dot-source this file to reach its functions without installing anything.
if ($env:AGENTMASTER_INSTALLER_LIBRARY -eq '1') { return }
try {
    Test-Windows
    Test-64BitHost
    Initialize-Net
    Import-AppxModule

    if (-not $DownloadDir) { $DownloadDir = Join-Path $env:TEMP 'Agentmaster-install' }
    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null

    Write-Host ""
    Write-Host "Agentmaster installer" -ForegroundColor White
    Write-Host ""

    # The older switches map onto the scope question.
    if (-not $Scope) {
        if ($Portable) { $Scope = 'User' }
        elseif ($Elevate) { $Scope = 'Package' }
    }

    $identity = Get-AmIdentity
    Show-IdentityNote -Identity $identity
    $state = Get-InstallState -Identity $identity

    if ($Uninstall) { Invoke-UninstallFlow -Identity $identity -State $state; return }

    $release = Get-Release -Headers (Get-GitHubHeaders)
    if (-not $Scope) { $Scope = Read-InstallScope -Identity $identity -State $state }
    switch ($Scope) {
        'User'    { Invoke-UserScopeFlow -Release $release -Identity $identity -State $state }
        'Machine' { Invoke-MachineScopeFlow -Release $release -Identity $identity -State $state }
        'Package' { Invoke-PackageScopeFlow -Release $release -Identity $identity -State $state }
        default   { Write-Warn 'Cancelled - nothing was installed.' }
    }
}
catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    # The way out of a rights problem - pointless after a per-user install or an uninstall failed.
    if ($Scope -and $Scope -ne 'User' -and -not $Uninstall) {
        Write-Host "       For an install without administrator rights: -Scope User" -ForegroundColor DarkGray
    }
    # Only set a failure exit code when invoked as a downloaded .ps1 file. A one-liner
    # (irm|iex or & ([scriptblock]::Create(...))) shares the caller's shell, where 'exit'
    # would CLOSE their window - so signal failure by message + LASTEXITCODE only.
    $global:LASTEXITCODE = 1
    if ($PSCommandPath) { exit 1 }
}
