# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Tests of the two installer scripts: tools\Install-Agentmaster.ps1 and the in-app payload
    src\cascadia\TerminalApp\AgentMaster\am-update.ps1 (doc/agentmaster/PROFILES.md section 2c).

.DESCRIPTION
    Plain PowerShell (no Pester), runs in Windows PowerShell 5.1 - the runtime both scripts target - and in
    PowerShell 7. It never downloads, installs or registers anything real:
      * both scripts are dot-sourced in LIBRARY mode (AGENTMASTER_INSTALLER_LIBRARY=1: their functions load,
        their main block does not run);
      * every file operation works in a scratch folder under %TEMP%;
      * the registration tests write into a scratch HKCU key and scratch shortcut folders (the Targets
        hashtable the core accepts), never into the real Apps & Features, verbs, PATH or Start menu;
      * everything scratch is removed in a finally block.
    What it pins: the SHARED INSTALL CORE is byte-identical in both files and lists every one of its
    functions for the phase export; both files are pure ASCII and parse; the manifest rules; the upgrade
    and uninstall remove ONLY what Agentmaster ships (a user's file, settings\, profile\, profile.path stay);
    an upgrade whose copy fails puts the previous version back (a source file held open forces the failure);
    the decision / pointer files; the write probe (a real deny ACL); the user and machine installs and their
    registration, written and removed - the all-users one named "Agentmaster (all users)", with no shared
    shortcut but an Active Setup component that gives each user icons of their own (kept by an update, removed
    with the copy, its icons swept from every user's folders); a user's own install taking the "Agentmaster" icon
    over, and giving its icons and taskbar pins back to the all-users copy when it is removed; the profile a
    per-user install uses and the data it copies in; when the old script's "Agentmaster (Portable)" shortcut
    is retired; the in-app updater's choices when the all-users copy cannot be updated; a phase script runs
    and reports back.

    -IncludeScheduledTask also runs Invoke-AmAsUser for the CURRENT user (a real, one-shot scheduled task,
    deleted at once) - meant for a disposable test desktop, not a workstation.

.PARAMETER IncludeScheduledTask
    Also exercise a real one-shot scheduled task (see above).

.OUTPUTS
    Prints one line per failed check and a summary; the exit code is the number of failed checks.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\tests\Test-AgentmasterInstaller.ps1
#>
param([switch]$IncludeScheduledTask)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $repo 'tools\Install-Agentmaster.ps1'
$updater = Join-Path $repo 'src\cascadia\TerminalApp\AgentMaster\am-update.ps1'
# Taken BEFORE the scripts are dot-sourced below: they define a $repo of their own (the GitHub repository name),
# which then replaces this one.
$portableInstallH = Join-Path $repo 'src\cascadia\TerminalApp\AgentMaster\PortableInstall.h'

$script:checks = 0
$script:failures = 0

<#
.SYNOPSIS
    Record one check: count it, and print it when it fails.
.PARAMETER Condition
    The checked condition.
.PARAMETER Message
    What it proves.
#>
function Assert {
    param([bool]$Condition, [string]$Message)
    $script:checks++
    if (-not $Condition) {
        $script:failures++
        Write-Host "  [FAIL] $Message" -ForegroundColor Red
    }
}

<#
.SYNOPSIS
    The text between the SHARED INSTALL CORE markers of a script (markers included).
.PARAMETER Path
    The script file.
.OUTPUTS
    [string] the block, or '' when the markers are missing.
#>
function Get-CoreBlock {
    param([string]$Path)
    $text = [IO.File]::ReadAllText($Path)
    $start = $text.IndexOf('# >>>>>>>> AGENTMASTER SHARED INSTALL CORE >>>>>>>>')
    $endMarker = '# <<<<<<<< AGENTMASTER SHARED INSTALL CORE <<<<<<<<'
    $end = $text.IndexOf($endMarker)
    if ($start -lt 0 -or $end -lt $start) { return '' }
    return $text.Substring($start, $end + $endMarker.Length - $start)
}

Write-Host 'Agentmaster installer scripts:'

# ---- the files themselves ------------------------------------------------------------------------------
foreach ($file in @($installer, $updater)) {
    $bytes = [IO.File]::ReadAllBytes($file)
    Assert (@($bytes | Where-Object { $_ -gt 127 }).Count -eq 0) "ascii: $(Split-Path -Leaf $file) is pure ASCII (Windows PowerShell 5.1 reads a BOM-less script as ANSI)"
    $tokens = $null
    $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors)
    Assert ($errors.Count -eq 0) "parse: $(Split-Path -Leaf $file) parses ($($errors.Count) error(s))"
}
$coreA = Get-CoreBlock $installer
$coreB = Get-CoreBlock $updater
Assert ($coreA.Length -gt 1000) 'core: Install-Agentmaster.ps1 carries the shared install core'
Assert ($coreA -ceq $coreB) 'core: the shared install core is byte-identical in both scripts (edit one, copy it to the other)'
{
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($coreA, [ref]$tokens, [ref]$errors)
    $defined = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false) | ForEach-Object { $_.Name }) | Sort-Object
    $listAst = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$script:AmCoreFunctionNames' }, $false) | Select-Object -First 1
    $listed = @()
    if ($listAst) { $listed = @($listAst.Right.FindAll({ param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst] }, $true) | ForEach-Object { $_.Value }) | Sort-Object }
    Assert ($listed.Count -gt 0) 'core: $script:AmCoreFunctionNames exists'
    $missing = @($defined | Where-Object { $listed -notcontains $_ })
    $extra = @($listed | Where-Object { $defined -notcontains $_ })
    Assert ($missing.Count -eq 0) ("core: every core function is exported to phase scripts (missing: " + ($missing -join ', ') + ")")
    Assert ($extra.Count -eq 0) ("core: the export list names only core functions (unknown: " + ($extra -join ', ') + ")")
}.Invoke()

# ---- load both scripts as libraries ----------------------------------------------------------------------
$env:AGENTMASTER_INSTALLER_LIBRARY = '1'
. $installer
$installerFunctions = @('Read-InstallScope', 'Invoke-UserScopeFlow', 'Invoke-MachineScopeFlow', 'Invoke-PackageScopeFlow', 'Invoke-UninstallFlow', 'Get-InstallState')
foreach ($f in $installerFunctions) { Assert ([bool](Get-Command $f -ErrorAction SilentlyContinue)) "library: Install-Agentmaster.ps1 defines $f" }
. $updater
foreach ($f in @('Invoke-PortableUpdate', 'Invoke-MachineInstall', 'Invoke-MachineUninstall', 'Invoke-RunAsMainUser', 'Invoke-SwitchToUserInstall')) {
    Assert ([bool](Get-Command $f -ErrorAction SilentlyContinue)) "library: am-update.ps1 defines $f"
}
# The REAL targets and user folders (read only), taken before any test below redirects USERPROFILE: the shell caches a
# known folder per process, and once looked up under the scratch profile, PowerShell 7 keeps answering '' for it.
$realUserTargets = Get-AmRegistrationTargets -Scope User
$realMachineTargets = Get-AmRegistrationTargets -Scope Machine
$realShellFolders = @(Get-AmUserShellFolders)
$realDesktop = [Environment]::GetFolderPath('DesktopDirectory')

$root = Join-Path $env:TEMP ('am-installer-tests-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$regRoot = 'HKCU:\Software\Agentmaster-InstallerTest-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$savedUserProfile = $env:USERPROFILE
New-Item -ItemType Directory -Force -Path $root | Out-Null
try {
    <#
    .SYNOPSIS
        Write a small text file (and its folder).
    .PARAMETER Path
        The file to write.
    .PARAMETER Text
        Its content.
    .NOTES
        Throws when the file cannot be written.
    #>
    function Write-TestFile { param([string]$Path, [string]$Text = 'x') New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null; [IO.File]::WriteAllText($Path, $Text) }
    <#
    .SYNOPSIS
        A fake release layout: an exe, a dll, a folder, the marker, the stamp, optionally a manifest.
    .PARAMETER Dir
        The layout folder to create.
    .PARAMETER Version
        The .am-version stamp it carries.
    .PARAMETER NoManifest
        Build it like a release made before the manifest existed.
    .PARAMETER Extra
        More top-level file names it ships (listed in its manifest).
    .NOTES
        Throws when a file cannot be written.
    #>
    function New-TestLayout {
        param([string]$Dir, [string]$Version = '9.9.9.0', [switch]$NoManifest, [string[]]$Extra = @())
        Write-TestFile (Join-Path $Dir 'Agentmaster.exe') 'exe'
        Write-TestFile (Join-Path $Dir 'TerminalApp.dll') 'dll'
        Write-TestFile (Join-Path $Dir 'Images\icon.png') 'png'
        Write-TestFile (Join-Path $Dir '.portable') ''
        Write-TestFile (Join-Path $Dir '.am-version') $Version
        foreach ($e in $Extra) { Write-TestFile (Join-Path $Dir $e) 'new' }
        if (-not $NoManifest) {
            $names = @('.am-manifest') + @(Get-ChildItem -LiteralPath $Dir -Force | ForEach-Object { $_.Name } | Where-Object { $_ -ne '.am-manifest' })
            [IO.File]::WriteAllText((Join-Path $Dir '.am-manifest'), ("# test manifest`n" + ($names -join "`n") + "`n"))
        }
    }
    <#
    .SYNOPSIS
        Registration targets inside the scratch key and folders (never the real ones).
    .PARAMETER Name
        A name that keeps this target set apart from the test's other sets.
    .OUTPUTS
        [hashtable] the Targets shape Register-AmInstall / Unregister-AmInstall accept.
    #>
    function New-TestTargets {
        param([string]$Name, [switch]$Machine)
        # The all-users shape names its entry AgentmasterAllUsers and remembers the old name (Get-AmRegistrationTargets).
        # Every other place the core may touch is a scratch one too: the Active Setup component and this user's marker
        # of it, the taskbar pins, the users' folders the all-users uninstall sweeps (ProfileFolders; without the key it
        # would read every real profile), and MachineDir ('' = no all-users copy a per-user uninstall hands its icons
        # to; without the key it would read the real HKLM).
        @{
            UninstallKey       = if ($Machine) { "$regRoot\$Name\Uninstall\AgentmasterAllUsers" } else { "$regRoot\$Name\Uninstall\Agentmaster" }
            LegacyUninstallKey = if ($Machine) { "$regRoot\$Name\Uninstall\Agentmaster" } else { '' }
            ClassesRoot        = "$regRoot\$Name\Classes"
            EnvKey             = "$regRoot\$Name\Environment"
            StartMenu          = Join-Path $root "$Name-startmenu"
            Desktop            = Join-Path $root "$Name-desktop"
            Broadcast          = $false
            ActiveSetupKey     = "$regRoot\$Name\ActiveSetup"
            UserActiveSetupKey = "$regRoot\$Name\UserActiveSetup"
            PinsDir            = if ($Machine) { '' } else { Join-Path $root "$Name-pins" }
            ProfileFolders     = @()
            MachineDir         = ''
        }
    }
    <#
    .SYNOPSIS
        Make a shortcut (WScript.Shell, like the core) - its folder too.
    .PARAMETER Lnk
        The .lnk file to write.
    .PARAMETER Target
        Its target.
    .PARAMETER Arguments
        Its arguments ('' for none).
    .NOTES
        Throws when the shortcut cannot be saved.
    #>
    function New-TestShortcut {
        param([string]$Lnk, [string]$Target, [string]$Arguments = '')
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Lnk) | Out-Null
        $s = (New-Object -ComObject WScript.Shell).CreateShortcut($Lnk)
        $s.TargetPath = $Target
        if ($Arguments) { $s.Arguments = $Arguments }
        $s.Save()
    }
    <#
    .SYNOPSIS
        A shortcut's target and arguments ('' / '' when it does not exist).
    .PARAMETER Lnk
        The .lnk file.
    .OUTPUTS
        [pscustomobject] Target, Arguments.
    #>
    function Get-TestShortcut {
        param([string]$Lnk)
        if (-not (Test-Path -LiteralPath $Lnk)) { return [pscustomobject]@{ Target = ''; Arguments = '' } }
        $s = (New-Object -ComObject WScript.Shell).CreateShortcut($Lnk)
        return [pscustomobject]@{ Target = [string]$s.TargetPath; Arguments = [string]$s.Arguments }
    }

    # ---- manifest names -----------------------------------------------------------------------------------
    Assert ((Test-SafeManifestName 'Agentmaster.exe') -and (Test-SafeManifestName '.am-version') -and (Test-SafeManifestName 'Microsoft.UI.Xaml')) 'manifest name: plain names are safe'
    Assert (-not (Test-SafeManifestName '') -and -not (Test-SafeManifestName '.') -and -not (Test-SafeManifestName '..')) 'manifest name: empty, . and .. are refused'
    Assert (-not (Test-SafeManifestName 'a\b') -and -not (Test-SafeManifestName 'a/b') -and -not (Test-SafeManifestName 'C:x') -and -not (Test-SafeManifestName '*.dll')) 'manifest name: separators, colons and wildcards are refused'
    Assert (-not (Test-SafeManifestName 'profile.') -and -not (Test-SafeManifestName 'settings ')) 'manifest name: a trailing dot or space is refused (Windows would strip it: "profile." names the preserved profile folder)'
    Assert (-not (Test-SafeManifestName 'NUL') -and -not (Test-SafeManifestName 'con.txt') -and -not (Test-SafeManifestName 'Com7') -and -not (Test-SafeManifestName 'lpt1.dll')) 'manifest name: DOS device names are refused'
    Assert ((Test-SafeManifestName 'console.dll') -and (Test-SafeManifestName 'com10') -and (Test-SafeManifestName '.portable')) 'manifest name: names that only start like a device stay allowed'
    $md = Join-Path $root 'manifest'
    Write-TestFile (Join-Path $md '.am-manifest') "# header`r`nAgentmaster.exe`r`n`r`nImages/`r`n  TerminalApp.dll  `r`n..`r`nsub\dir`r`nagentmaster.EXE`r`n"
    $names = @(Read-AmManifest -Dir $md)
    Assert ($names.Count -eq 3) "manifest parse: three names, comments / blanks / unsafe / case-duplicates skipped (got $($names.Count))"
    Assert (($names -join '|') -ceq 'Agentmaster.exe|Images|TerminalApp.dll') 'manifest parse: file order kept, the folder slash dropped, trimmed'
    Assert (@(Read-AmManifest -Dir (Join-Path $root 'nowhere')).Count -eq 0) 'manifest parse: no manifest -> no names'

    # ---- shipped names: the manifest, else the legacy list --------------------------------------------------
    $legacy = Join-Path $root 'legacy'
    foreach ($n in 'Agentmaster.exe', 'Microsoft.Terminal.Control.dll', 'resources.pri', '.portable', 'notes.txt', 'my-tool.exe') { Write-TestFile (Join-Path $legacy $n) }
    Write-TestFile (Join-Path $legacy 'profile\sessions.json') '{}'
    $shipped = @(Get-AmShippedNames -Dir $legacy)
    Assert ($shipped -contains 'Agentmaster.exe' -and $shipped -contains 'Microsoft.Terminal.Control.dll' -and $shipped -contains 'resources.pri' -and $shipped -contains '.portable') 'shipped (no manifest): the legacy list and pattern match what releases ship'
    Assert ($shipped -notcontains 'notes.txt' -and $shipped -notcontains 'my-tool.exe') 'shipped (no manifest): a user file is not ours - not even another .exe'
    Assert ($shipped -notcontains 'profile') 'shipped: profile\ is never a shipped name'

    # ---- the Apps & Features version text: the app's own format (PortableInstall.h FormatVersion) ----------
    Assert ((Format-AmVersion '0.8.2.0') -eq '0.8.2' -and (Format-AmVersion '0.8.2') -eq '0.8.2') 'version text: a zero fourth part is dropped, like the app writes it'
    Assert ((Format-AmVersion '1.2.3.4') -eq '1.2.3.4' -and (Format-AmVersion 'v0.8.2-prerelease-nightly') -eq '0.8.2' -and (Format-AmVersion '0.8') -eq '0.8.0') 'version text: a real fourth part stays; a tag and a two-part version normalize'
    Assert ((Format-AmVersion 'not a version') -eq 'not a version') 'version text: anything else passes through'

    # ---- why an elevation failed: the Win32 code inside Start-Process's wrapped exception ------------------
    $policy = New-Object System.InvalidOperationException('This command cannot be run due to the error: blocked by group policy', (New-Object System.ComponentModel.Win32Exception(1260)))
    $policyRecord = New-Object System.Management.Automation.ErrorRecord($policy, 'InvalidOperationException', 'NotSpecified', $null)
    Assert ((Get-AmWin32ErrorCode -ErrorRecord $policyRecord -Default 1223) -eq 1260) 'elevation failure: a policy refusal (1260) is read from the wrapped Win32 error'
    $plainRecord = New-Object System.Management.Automation.ErrorRecord((New-Object System.InvalidOperationException('no win32 inside')), 'x', 'NotSpecified', $null)
    Assert ((Get-AmWin32ErrorCode -ErrorRecord $plainRecord -Default 1223) -eq 1223 -and (Get-AmWin32ErrorCode -ErrorRecord $null) -eq 0) 'elevation failure: no Win32 error -> the default (declined)'

    # ---- the decision and pointer files ---------------------------------------------------------------------
    $dd = Join-Path $root 'decision'
    New-Item -ItemType Directory -Force -Path $dd | Out-Null
    Assert ((Get-AmInstallDecision -Dir $dd) -eq '') 'decision: absent -> none'
    foreach ($v in 'installed', 'machine', 'portable') {
        Set-AmInstallDecision -Dir $dd -Value $v
        Assert ((Get-AmInstallDecision -Dir $dd) -eq $v) "decision: $v round-trips"
    }
    [IO.File]::WriteAllText((Join-Path $dd 'install.path'), "# c`r`n  HERE  `r`n")
    Assert ((Get-AmInstallDecision -Dir $dd) -eq 'installed') "decision: the older 'here' spelling reads as installed"
    Set-AmInstallDecision -Dir $dd -Value 'C:\Users\x\.agentmaster\bin'
    Assert ((Get-AmInstallDecision -Dir $dd) -eq 'C:\Users\x\.agentmaster\bin') 'decision: a launcher stub folder round-trips'
    [IO.File]::WriteAllText((Join-Path $dd 'install.path'), "relative\dir`n")
    Assert ((Get-AmInstallDecision -Dir $dd) -eq '') 'decision: a relative value reads as none (re-ask)'
    Assert (Set-AmProfilePointer -Dir $dd -ProfileDir (Join-Path $root 'prof-a')) 'pointer: written when absent'
    Assert (-not (Set-AmProfilePointer -Dir $dd -ProfileDir (Join-Path $root 'prof-b'))) 'pointer: an existing pointer is kept (an upgrade keeps the choice)'
    Assert (([IO.File]::ReadAllText((Join-Path $dd 'profile.path'))).Contains('prof-a')) 'pointer: the first choice stays'

    # ---- the write probe (a real deny ACL, the way Program Files treats a standard user) --------------------
    $wp = Join-Path $root 'writable'
    New-Item -ItemType Directory -Force -Path $wp | Out-Null
    Assert (Test-AmDirWritable $wp) 'write probe: a scratch folder is writable'
    Assert (-not (Test-AmDirWritable (Join-Path $root 'missing'))) 'write probe: a missing folder is not'
    $ro = Join-Path $root 'readonly'
    New-Item -ItemType Directory -Force -Path $ro | Out-Null
    # icacls, not Get-Acl / Set-Acl: those live in Microsoft.PowerShell.Security, and a Windows PowerShell 5.1
    # started from a PowerShell 7 environment (an inherited PSModulePath) finds PS7's copy of that module
    # first and cannot load it. *S-1-1-0 = Everyone; WD / AD on a folder = create files / create folders.
    $icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
    $null = & $icacls $ro /deny '*S-1-1-0:(WD,AD)'
    Assert ($LASTEXITCODE -eq 0) 'write probe: the deny ACL is applied (icacls)'
    Assert (-not (Test-AmDirWritable $ro)) 'write probe: a folder that denies creating files is not writable'
    $null = & $icacls $ro /remove:d '*S-1-1-0'

    # ---- the layout install / upgrade: only what Agentmaster ships ------------------------------------------
    $src = Join-Path $root 'release-new'
    New-TestLayout -Dir $src -Version '9.9.9.0' -Extra @('new.dll')
    $dst = Join-Path $root 'install-a'
    New-TestLayout -Dir $dst -Version '1.0.0.0' -Extra @('old.dll')
    Write-TestFile (Join-Path $dst 'my-notes.txt') 'mine'
    Write-TestFile (Join-Path $dst 'profile\sessions.json') '{}'
    Write-TestFile (Join-Path $dst 'settings\settings.json') '{}'
    Write-TestFile (Join-Path $dst 'profile.path') "profile`n"
    Set-AmInstallDecision -Dir $dst -Value 'installed'
    Install-AmLayout -SourceDir $src -DestDir $dst
    Assert (([IO.File]::ReadAllText((Join-Path $dst 'Agentmaster.exe'))) -eq 'exe' -and (Test-Path (Join-Path $dst 'new.dll'))) 'upgrade: the new layout is laid in'
    Assert (-not (Test-Path (Join-Path $dst 'old.dll'))) 'upgrade: a file only the OLD manifest lists is removed (no stale files)'
    Assert ((Get-AmInstalledVersion -Dir $dst) -eq [version]'9.9.9.0') 'upgrade: the new stamp'
    Assert (Test-Path (Join-Path $dst 'my-notes.txt')) "upgrade: a user's own file in the folder is NEVER removed"
    Assert ((Test-Path (Join-Path $dst 'profile\sessions.json')) -and (Test-Path (Join-Path $dst 'settings\settings.json'))) 'upgrade: profile\ and settings\ stay'
    Assert ((Test-Path (Join-Path $dst 'profile.path')) -and (Get-AmInstallDecision -Dir $dst) -eq 'installed') 'upgrade: profile.path and install.path stay'
    $legacyInstall = Join-Path $root 'install-legacy'
    New-TestLayout -Dir $legacyInstall -Version '0.7.9.0' -NoManifest -Extra @('Microsoft.Terminal.UI.dll')
    Write-TestFile (Join-Path $legacyInstall 'other-app.exe') 'not ours'
    Install-AmLayout -SourceDir $src -DestDir $legacyInstall
    Assert (-not (Test-Path (Join-Path $legacyInstall 'Microsoft.Terminal.UI.dll'))) 'upgrade (old install, no manifest): a legacy-listed file is removed'
    Assert (Test-Path (Join-Path $legacyInstall 'other-app.exe')) 'upgrade (old install, no manifest): an unknown .exe of the user stays'
    Assert (Test-Path (Join-Path $legacyInstall '.am-manifest')) 'upgrade: the new manifest arrives with the layout'

    # ---- the download mark: an install's own files lose it, a user's own file keeps it ----------------------
    # A browser marks a download (the Zone.Identifier stream) and Explorer's zip extraction copies the mark onto
    # every extracted file; Copy-Item carries it into the install (60 of 63 installed files in a test desktop).
    $marked = Join-Path $root 'release-marked'
    New-TestLayout -Dir $marked -Version '9.9.9.1'
    $zone = "[ZoneTransfer]`r`nZoneId=3`r`n"
    foreach ($f in 'Agentmaster.exe', 'TerminalApp.dll', 'Images\icon.png') { Set-Content -LiteralPath (Join-Path $marked $f) -Stream Zone.Identifier -Value $zone }
    $markDst = Join-Path $root 'install-marks'
    Write-TestFile (Join-Path $markDst 'my-download.zip') 'mine'
    Set-Content -LiteralPath (Join-Path $markDst 'my-download.zip') -Stream Zone.Identifier -Value $zone
    Assert ($null -ne (Get-Item -LiteralPath (Join-Path $marked 'Agentmaster.exe') -Stream Zone.Identifier -ErrorAction SilentlyContinue)) 'download mark: the test layout carries the mark (alternate streams work in the scratch folder)'
    Install-AmLayout -SourceDir $marked -DestDir $markDst
    $stillMarked = @('Agentmaster.exe', 'TerminalApp.dll', 'Images\icon.png' | Where-Object { Get-Item -LiteralPath (Join-Path $markDst $_) -Stream Zone.Identifier -ErrorAction SilentlyContinue })
    Assert ($stillMarked.Count -eq 0) "download mark: every installed file loses it (still marked: $($stillMarked -join ', '))"
    Assert ($null -ne (Get-Item -LiteralPath (Join-Path $markDst 'my-download.zip') -Stream Zone.Identifier -ErrorAction SilentlyContinue)) "download mark: a user's own file in the install folder keeps it"

    # ---- a failed upgrade puts the previous version back (2026-10-21) ----------------------------------------
    # Before, the old files were deleted before the copy, and a copy that failed half way left a folder with
    # neither version: the in-app update then had nothing to start. A source file held open with no sharing
    # makes the copy fail on its LAST entry (Get-ChildItem lists alphabetically), after every other entry of
    # the new layout was laid down.
    $failSrc = Join-Path $root 'release-fails'
    New-TestLayout -Dir $failSrc -Version '9.9.9.2' -Extra @('new-only.dll', 'zz-locked.dll')
    $failDst = Join-Path $root 'install-restore'
    New-TestLayout -Dir $failDst -Version '1.0.0.0' -Extra @('old-only.dll')
    [IO.File]::WriteAllText((Join-Path $failDst 'Agentmaster.exe'), 'old exe')
    Write-TestFile (Join-Path $failDst 'my-notes.txt') 'mine'
    Write-TestFile (Join-Path $failDst 'profile\sessions.json') '{}'
    Write-TestFile (Join-Path $failDst '.am-previous-0badc0de\stale.dll') 'left by an interrupted upgrade'
    $lock = [IO.File]::Open((Join-Path $failSrc 'zz-locked.dll'), 'Open', 'Read', 'None')
    $threw = $false
    try { Install-AmLayout -SourceDir $failSrc -DestDir $failDst } catch { $threw = $true } finally { $lock.Dispose() }
    Assert $threw 'restore: a copy that fails half way throws (the caller starts the previous version again)'
    Assert (([IO.File]::ReadAllText((Join-Path $failDst 'Agentmaster.exe'))) -eq 'old exe') 'restore: the previous exe is back'
    Assert ((Test-Path (Join-Path $failDst 'old-only.dll')) -and (Test-Path (Join-Path $failDst 'Images\icon.png'))) 'restore: a file and a folder only the old version had are back'
    Assert (-not (Test-Path (Join-Path $failDst 'new-only.dll'))) 'restore: what the failed copy laid down is gone'
    Assert ((Get-AmInstalledVersion -Dir $failDst) -eq [version]'1.0.0.0') 'restore: the previous version stamp is back'
    Assert ((Test-Path (Join-Path $failDst 'my-notes.txt')) -and (Test-Path (Join-Path $failDst 'profile\sessions.json'))) "restore: a user's own file and profile\ never moved"
    Assert (@(Get-ChildItem -LiteralPath $failDst -Directory -Force -Filter '.am-previous-*').Count -eq 0) 'restore: no set-aside folder is left (the stale one of an earlier run included)'
    Install-AmLayout -SourceDir $failSrc -DestDir $failDst
    Assert (([IO.File]::ReadAllText((Join-Path $failDst 'Agentmaster.exe'))) -eq 'exe' -and (Test-Path (Join-Path $failDst 'new-only.dll')) -and -not (Test-Path (Join-Path $failDst 'old-only.dll'))) 'restore: the same upgrade succeeds once the file is free'
    Assert (@(Get-ChildItem -LiteralPath $failDst -Directory -Force -Filter '.am-previous-*').Count -eq 0) 'restore: a successful upgrade leaves no set-aside folder'

    # ---- copying a profile's data: copy-if-absent, the app's MigrateProfileData exclusions --------------------
    $pFrom = Join-Path $root 'profile-from'
    $pTo = Join-Path $root 'profile-to'
    Write-TestFile (Join-Path $pFrom 'sessions.json') 'from'
    Write-TestFile (Join-Path $pFrom 'windows\w1.json') 'w1'
    Write-TestFile (Join-Path $pFrom 'settings.json') 'from settings'
    Write-TestFile (Join-Path $pFrom 'locks\build-launch') 'x'
    Write-TestFile (Join-Path $pFrom 'shim\claude.cmd') 'x'
    Write-TestFile (Join-Path $pFrom 'bin\Agentmaster.exe') 'x'
    Write-TestFile (Join-Path $pFrom 'bridge.json') 'x'
    Write-TestFile (Join-Path $pFrom 'state.tmp') 'x'
    Write-TestFile (Join-Path $pTo 'settings.json') 'already there'
    $nCopied = Copy-AmProfileData -From $pFrom -To $pTo
    Assert ($nCopied -eq 2 -and (Test-Path (Join-Path $pTo 'sessions.json')) -and (Test-Path (Join-Path $pTo 'windows\w1.json'))) "profile copy: the data arrives, a nested file included (copied $nCopied)"
    Assert (([IO.File]::ReadAllText((Join-Path $pTo 'settings.json'))) -eq 'already there') 'profile copy: a file the target holds is never overwritten'
    Assert (-not (Test-Path (Join-Path $pTo 'locks')) -and -not (Test-Path (Join-Path $pTo 'shim')) -and -not (Test-Path (Join-Path $pTo 'bin')) -and -not (Test-Path (Join-Path $pTo 'bridge.json')) -and -not (Test-Path (Join-Path $pTo 'state.tmp'))) 'profile copy: locks\, shim\, bin\, bridge.json and *.tmp stay behind'
    Assert ((Copy-AmProfileData -From $pFrom -To $pFrom) -eq 0 -and (Copy-AmProfileData -From (Join-Path $root 'nowhere') -To $pTo) -eq 0) 'profile copy: onto itself, or from nothing, copies nothing'

    # ---- the per-user install: shape, registration, profile, kept-portable ----------------------------------
    $env:USERPROFILE = Join-Path $root 'home'
    New-Item -ItemType Directory -Force -Path $env:USERPROFILE | Out-Null
    # The mapped profiles live in the scratch folder too: the install CREATES the profile folder it points
    # profile.path at (a map naming D:\data\... created that folder on the developer's real D: drive).
    $mappedProfile = Join-Path $root 'data\am-profile'
    $otherProfile = Join-Path $root 'data\unpackaged-profile'
    if (-not ($mappedProfile.StartsWith($root) -and $otherProfile.StartsWith($root))) { throw 'test bug: a mapped profile outside the scratch folder would be created on this PC' }
    [IO.File]::WriteAllText((Join-Path $env:USERPROFILE '.agentmaster.profiles'), "# map`nAgentmaster_56k4f06dsfp9r=$mappedProfile`nUnpackaged=$otherProfile`n")
    Assert ((Get-AmUserProfileDir) -eq $mappedProfile) "user profile: the MSIX package's own choice is adopted (the same data after a switch)"
    $tu = New-TestTargets 'user'
    $userDir = Join-Path $root 'user-install'
    $registeredNow = Install-AmUserCopy -LayoutDir $src -DestDir $userDir -Version '9.9.9.0' -Targets $tu
    Assert ($registeredNow -eq $true) 'user install: reports that the folder is a registered install'
    Assert ((Get-AmInstallDecision -Dir $userDir) -eq 'installed') 'user install: install.path = installed (the app then never re-asks)'
    Assert (([IO.File]::ReadAllText((Join-Path $userDir 'profile.path'))).Contains($mappedProfile)) 'user install: profile.path -> the adopted profile'
    Assert (Test-Path -LiteralPath $mappedProfile) 'user install: the adopted profile folder exists'
    Assert (-not (Test-Path -LiteralPath $otherProfile)) 'user install: only the release family''s profile is used (the Unpackaged entry is not)'
    $u = Get-ItemProperty -LiteralPath $tu.UninstallKey
    Assert ($u.UninstallString -eq ('"' + (Join-Path $userDir 'Agentmaster.exe') + '" --uninstall-portable')) 'user install: the Apps & Features UninstallString is the app''s own --uninstall-portable'
    Assert ($u.InstallLocation -eq $userDir -and $u.DisplayVersion -eq '9.9.9' -and $u.DisplayName -eq 'Agentmaster') 'user install: InstallLocation, DisplayVersion (the app''s format), DisplayName'
    $cmd = (Get-Item -LiteralPath "$($tu.ClassesRoot)\Directory\Background\shell\Agentmaster\command").GetValue('')
    Assert ($cmd -eq ('"' + (Join-Path $userDir 'Agentmaster.exe') + '" -d "%V"')) 'user install: the folder-background verb opens the folder (-d "%V")'
    $path = (Get-Item -LiteralPath $tu.EnvKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    Assert ($path -eq $userDir) 'user install: the folder is on the (scratch) PATH'
    Assert ((Test-Path (Join-Path $tu.StartMenu 'Agentmaster.lnk')) -and (Test-Path (Join-Path $tu.Desktop 'Agentmaster.lnk'))) 'user install: Start-menu and desktop shortcuts'
    # The shared copy's per-user icon carries the same name: a user's own install takes it over, made ANEW (a shortcut
    # WScript.Shell saves over keeps the old one's other properties - its arguments here, its AppUserModelID for real).
    New-TestShortcut -Lnk (Join-Path $tu.Desktop 'Agentmaster.lnk') -Target (Join-Path $root 'all-users-copy\Agentmaster.exe') -Arguments '--from-the-shared-icon'
    $null = Register-AmInstall -Scope User -Dir $userDir -Version '9.9.9.0' -Targets $tu
    $fresh = Get-TestShortcut (Join-Path $tu.Desktop 'Agentmaster.lnk')
    Assert ($fresh.Target -eq (Join-Path $userDir 'Agentmaster.exe') -and $fresh.Arguments -eq '') 'user install: an "Agentmaster" icon already there (the shared copy''s) is replaced by a fresh one of its own'
    $path2 = (Get-Item -LiteralPath $tu.EnvKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    Assert ($path2 -eq $userDir) 'user install: a second registration does not add the PATH entry twice'
    $kept = Join-Path $root 'kept-portable'
    New-TestLayout -Dir $kept -Version '1.0.0.0'
    Set-AmInstallDecision -Dir $kept -Value 'portable'
    $tk = New-TestTargets 'kept'
    $keptRegistered = Install-AmUserCopy -LayoutDir $src -DestDir $kept -Version '9.9.9.0' -Targets $tk
    Assert ((Get-AmInstallDecision -Dir $kept) -eq 'portable' -and -not (Test-Path -LiteralPath $tk.UninstallKey)) 'user install: a folder kept portable by choice only gets its files updated'
    Assert ($keptRegistered -eq $false) 'user install: ...and reports it was not registered (its old Start-menu entry is then kept)'

    # The profile a switch keeps (-ProfileDir: the in-app updater passes the profile the app used), and the data a
    # folder that never chose a profile kept in <folder>\profile (the old script's folder, run with "Ask me later").
    $switchProfile = Join-Path $root 'data\the-app-profile'
    $oldScriptDir = Join-Path $root 'old-script-copy'
    New-TestLayout -Dir $oldScriptDir -Version '0.7.10.0' -NoManifest
    Write-TestFile (Join-Path $oldScriptDir 'profile\sessions.json') 'from the old folder'
    $ts = New-TestTargets 'switch'
    $null = Install-AmUserCopy -LayoutDir $src -DestDir $oldScriptDir -Version '9.9.9.0' -Targets $ts -ProfileDir $switchProfile
    Assert (([IO.File]::ReadAllText((Join-Path $oldScriptDir 'profile.path'))).Contains($switchProfile)) 'user install: -ProfileDir is the profile the installed copy uses'
    Assert ((Test-Path (Join-Path $switchProfile 'sessions.json')) -and (Test-Path (Join-Path $oldScriptDir 'profile\sessions.json'))) "user install: a folder's own profile data is copied into that profile (and stays where it was)"
    Assert ((Get-AmInstallDecision -Dir $oldScriptDir) -eq 'installed') 'user install: the old script''s folder becomes a registered install'

    # The old script's Start-menu entry "Agentmaster (Portable)": retired only when it leads to the install.
    $progs = Join-Path $root 'legacy-programs'
    New-Item -ItemType Directory -Force -Path $progs | Out-Null
    $wsh = New-Object -ComObject WScript.Shell
    $legacyLnk = Join-Path $progs 'Agentmaster (Portable).lnk'
    $mk = { param($target) $s = $wsh.CreateShortcut($legacyLnk); $s.TargetPath = $target; $s.Save() }
    & $mk (Join-Path $oldScriptDir 'Agentmaster.exe')
    Assert (Remove-AmLegacyScriptShortcut -InstallDir $oldScriptDir -ProgramsDir $progs) 'old shortcut: retired when it points into the registered install'
    Assert (-not (Test-Path -LiteralPath $legacyLnk)) 'old shortcut: ...and it is gone'
    $stubDir = Join-Path $root 'old-script-stub'
    Write-TestFile (Join-Path $stubDir 'Agentmaster.exe') 'stub'
    Set-AmInstallDecision -Dir $stubDir -Value $oldScriptDir
    & $mk (Join-Path $stubDir 'Agentmaster.exe')
    Assert (Remove-AmLegacyScriptShortcut -InstallDir $oldScriptDir -ProgramsDir $progs) 'old shortcut: retired when it points into a launcher stub that names the install'
    & $mk (Join-Path $root 'some-other-copy\Agentmaster.exe')
    Assert (-not (Remove-AmLegacyScriptShortcut -InstallDir $oldScriptDir -ProgramsDir $progs) -and (Test-Path -LiteralPath $legacyLnk)) 'old shortcut: kept when it leads to another copy (before, it was removed whatever it pointed at)'
    Assert (-not (Remove-AmLegacyScriptShortcut -InstallDir $oldScriptDir -ProgramsDir (Join-Path $root 'no-programs'))) 'old shortcut: none there -> nothing to retire'

    # ---- the machine install: refuses an old layout, writes machine, no pointer -----------------------------
    $tm = New-TestTargets 'machine' -Machine
    $real = Get-AmRegistrationTargets -Scope Machine
    Assert ($real.UninstallKey -like '*\Uninstall\AgentmasterAllUsers' -and $real.LegacyUninstallKey -like '*\Uninstall\Agentmaster' -and (Get-AmRegistrationTargets -Scope User).UninstallKey -like 'HKCU:*\Uninstall\Agentmaster') 'targets: the all-users entry has its own key name (a shared one hid the per-user entry in Settings)'
    # Under the scratch USERPROFILE (set above) the user's AppData does not exist, and .NET answers '' for it in
    # PowerShell 7: the targets must still come back, with no pins folder (before, Join-Path threw on the '').
    $scratchUser = $null
    try { $scratchUser = Get-AmRegistrationTargets -Scope User } catch { $scratchUser = $null }
    Assert ($null -ne $scratchUser -and $scratchUser.UninstallKey -like 'HKCU:*') 'targets: a profile whose AppData does not exist still has targets (no pins folder then)'
    # The real ones (read at the top, under the real profile).
    Assert ($realMachineTargets.ActiveSetupKey -eq ('HKLM:\SOFTWARE\Microsoft\Active Setup\Installed Components\' + $script:AmActiveSetupId) -and $realUserTargets.UserActiveSetupKey -eq ('HKCU:\SOFTWARE\Microsoft\Active Setup\Installed Components\' + $script:AmActiveSetupId)) 'targets: the Active Setup component (HKLM) and this user''s marker of it (HKCU)'
    Assert ($realMachineTargets.PinsDir -eq '' -and $realUserTargets.PinsDir -like '*\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar') 'targets: taskbar pins are a user''s own (none for the all-users scope)'
    $pinstallH = [IO.File]::ReadAllText($portableInstallH)
    Assert ($script:AmActiveSetupId -match '^\{[0-9A-F-]{36}\}$' -and $pinstallH.Contains('Installed Components\\' + $script:AmActiveSetupId + '"')) 'targets: the script''s Active Setup id is the app''s (PortableInstall.h kActiveSetupKey)'
    $asv = New-AmActiveSetupVersion
    $asParts = @($asv -split ',')
    Assert ($asParts.Count -eq 4 -and $asParts[0] -eq '1' -and (@($asParts | Where-Object { $_ -notmatch '^\d{1,5}$' -or [int]$_ -gt 65535 }).Count -eq 0)) "active setup version: 1,<year>,<MMdd>,<HHmm>, each part a number below 65536 (got $asv)"
    Assert ($realDesktop -and @($realShellFolders | Where-Object { $_ -ieq $realDesktop.TrimEnd('\') }).Count -eq 1) 'user folders: the sweep list holds this user''s own desktop, once (one path per item, no trailing backslash)'
    $oldLayout = Join-Path $root 'release-old'
    New-TestLayout -Dir $oldLayout -Version '0.7.9.0' -NoManifest
    $threw = $false
    try { Install-AmMachineCopy -LayoutDir $oldLayout -DestDir (Join-Path $root 'machine-old') -Version '0.7.9.0' -Targets $tm } catch { $threw = $true }
    Assert $threw 'machine install: a release without a manifest is refused (its app would not know install.path = machine)'
    $machineDir = Join-Path $root 'machine-install'
    # The shared shortcuts older all-users registrations made - "Agentmaster.lnk" (the first build with all-users
    # installs) and "Agentmaster (all users).lnk" (2026-10-21) - go when they point into the folder (since 2026-10-22
    # every user gets icons of their own instead); one leading elsewhere is not touched.
    New-TestShortcut -Lnk (Join-Path $tm.StartMenu 'Agentmaster.lnk') -Target (Join-Path $machineDir 'Agentmaster.exe')
    New-TestShortcut -Lnk (Join-Path $tm.StartMenu 'Agentmaster (all users).lnk') -Target (Join-Path $machineDir 'Agentmaster.exe')
    New-TestShortcut -Lnk (Join-Path $tm.Desktop 'Agentmaster (all users).lnk') -Target (Join-Path $machineDir 'Agentmaster.exe')
    New-TestShortcut -Lnk (Join-Path $tm.Desktop 'Agentmaster.lnk') -Target (Join-Path $root 'a-users-own-copy\Agentmaster.exe')
    # ...and its Apps & Features entry under the old key name, which the renamed key replaces.
    New-Item -Path $tm.LegacyUninstallKey -Force | Out-Null
    New-ItemProperty -LiteralPath $tm.LegacyUninstallKey -Name 'InstallLocation' -Value $machineDir -PropertyType String -Force | Out-Null
    Install-AmMachineCopy -LayoutDir $src -DestDir $machineDir -Version '9.9.9.0' -Targets $tm
    Assert (-not (Test-Path -LiteralPath $tm.LegacyUninstallKey)) 'machine install: an entry under the old key name that describes this folder is replaced by the renamed one'
    Assert ((Get-AmInstallDecision -Dir $machineDir) -eq 'machine') 'machine install: install.path = machine'
    Assert (-not (Test-Path (Join-Path $machineDir 'profile.path'))) 'machine install: no profile.path (each user resolves their own)'
    Assert ((Get-ItemProperty -LiteralPath $tm.UninstallKey).InstallLocation -eq $machineDir) 'machine install: registered'
    Assert ((Get-ItemProperty -LiteralPath $tm.UninstallKey).DisplayVersion -eq '9.9.9') "machine install: Apps & Features shows the installed version (the Active Setup step once overwrote it - PowerShell names ignore case - found in the test desktop; got $((Get-ItemProperty -LiteralPath $tm.UninstallKey).DisplayVersion))"
    Assert ((Get-ItemProperty -LiteralPath $tm.UninstallKey).DisplayName -eq 'Agentmaster (all users)') 'machine install: Apps & Features reads "Agentmaster (all users)" (a user''s own copy reads "Agentmaster")'
    Assert (-not (Test-Path (Join-Path $tm.StartMenu 'Agentmaster (all users).lnk')) -and -not (Test-Path (Join-Path $tm.Desktop 'Agentmaster (all users).lnk'))) 'machine install: no shared shortcut (a user without administrator rights could neither remove nor re-point it); the 2026-10-21 ones go'
    Assert (-not (Test-Path (Join-Path $tm.StartMenu 'Agentmaster.lnk'))) 'machine install: an older shared Agentmaster.lnk pointing into the folder goes'
    Assert ((Get-TestShortcut (Join-Path $tm.Desktop 'Agentmaster.lnk')).Target -eq (Join-Path $root 'a-users-own-copy\Agentmaster.exe')) 'machine install: an Agentmaster.lnk leading elsewhere stays'
    $asItem = Get-Item -LiteralPath $tm.ActiveSetupKey
    Assert ([string]$asItem.GetValue('StubPath') -eq ('"' + (Join-Path $machineDir 'Agentmaster.exe') + '" --register-user')) 'machine install: the Active Setup component runs "<exe>" --register-user for every user (their own icons, once)'
    Assert ([int]$asItem.GetValue('IsInstalled') -eq 1 -and [int]$asItem.GetValue('DontAsk') -eq 2 -and [string]$asItem.GetValue('') -eq 'Agentmaster (all users)') 'machine install: ...installed, never asked about, named'
    $firstVersion = [string]$asItem.GetValue('Version')
    Assert ($firstVersion -match '^1,\d{4},\d{1,4},\d{1,4}$') "machine install: a fresh install writes a new Active Setup version (got $firstVersion)"
    New-ItemProperty -LiteralPath $tm.ActiveSetupKey -Name 'Version' -Value '1,2000,101,1' -PropertyType String -Force | Out-Null
    $null = Register-AmInstall -Scope Machine -Dir $machineDir -Version '9.9.9.0' -Targets $tm
    Assert ([string](Get-Item -LiteralPath $tm.ActiveSetupKey).GetValue('Version') -eq '1,2000,101,1') 'machine install: a re-registration (an update) keeps the version, so an icon a user deleted does not come back'
    Assert ((Get-ItemProperty -LiteralPath $tm.UninstallKey).DisplayVersion -eq '9.9.9') 'machine install: ...and Apps & Features still shows the installed version'
    Assert ((Get-AmRegistrationName -Scope User) -eq 'Agentmaster' -and (Get-AmRegistrationName -Scope Machine) -eq 'Agentmaster (all users)') 'registration name: per scope'

    # ---- uninstall: only what is ours, and only registrations pointing into the folder ----------------------
    $other = Join-Path $root 'some-other-install'
    $null = New-Item -ItemType Directory -Force -Path $other
    New-Item -Path "$($tu.ClassesRoot)\Drive\shell\Agentmaster\command" -Force | Out-Null
    Set-Item -LiteralPath "$($tu.ClassesRoot)\Drive\shell\Agentmaster\command" -Value ('"' + (Join-Path $other 'Agentmaster.exe') + '" -d "%1"')
    Write-TestFile (Join-Path $userDir 'my-notes.txt') 'mine'
    Write-TestFile (Join-Path $userDir 'profile\sessions.json') '{}'
    $left = @(Uninstall-AmCopy -Scope User -Dir $userDir -Targets $tu)
    Assert (-not (Test-Path (Join-Path $userDir 'Agentmaster.exe')) -and -not (Test-Path (Join-Path $userDir 'install.path'))) 'uninstall: the shipped files and install.path are removed'
    Assert ((Test-Path (Join-Path $userDir 'my-notes.txt')) -and (Test-Path (Join-Path $userDir 'profile\sessions.json')) -and (Test-Path (Join-Path $userDir 'profile.path'))) "uninstall: a user's file, profile\ and profile.path stay"
    Assert ($left -contains 'my-notes.txt' -and $left -contains 'profile') 'uninstall: it reports what it kept'
    Assert (-not (Test-Path -LiteralPath $tu.UninstallKey) -and -not (Test-Path (Join-Path $tu.StartMenu 'Agentmaster.lnk'))) 'uninstall: its Apps & Features entry and shortcuts go'
    Assert (-not (Test-Path -LiteralPath "$($tu.ClassesRoot)\Directory\shell\Agentmaster")) 'uninstall: its verbs go'
    Assert (Test-Path -LiteralPath "$($tu.ClassesRoot)\Drive\shell\Agentmaster") 'uninstall: a verb that points at ANOTHER install stays'
    Assert (((Get-Item -LiteralPath $tu.EnvKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')) -eq '') 'uninstall: its PATH entry goes'
    Write-TestFile (Join-Path $machineDir '.am-previous-12345678\TerminalApp.dll') 'left by an interrupted upgrade'
    # An old-name entry describing ANOTHER folder is not this copy's to remove.
    New-Item -Path $tm.LegacyUninstallKey -Force | Out-Null
    New-ItemProperty -LiteralPath $tm.LegacyUninstallKey -Name 'InstallLocation' -Value (Join-Path $root 'another-all-users-copy') -PropertyType String -Force | Out-Null
    # The icons it gave the users of this PC: one user's (into the folder) is swept, another user's own copy's is kept.
    $user1Desk = Join-Path $root 'users\one\Desktop'
    $user2Progs = Join-Path $root 'users\two\Programs'
    New-TestShortcut -Lnk (Join-Path $user1Desk 'Agentmaster.lnk') -Target (Join-Path $machineDir 'Agentmaster.exe')
    New-TestShortcut -Lnk (Join-Path $user2Progs 'Agentmaster.lnk') -Target (Join-Path $root 'users\two\own-copy\Agentmaster.exe')
    $tm.ProfileFolders = @($user1Desk, $user2Progs, (Join-Path $root 'users\three\no-desktop'))
    $null = Uninstall-AmCopy -Scope Machine -Dir $machineDir -Targets $tm
    Assert (-not (Test-Path -LiteralPath $tm.UninstallKey) -and (Test-Path -LiteralPath $tm.LegacyUninstallKey)) 'uninstall (all users): its entry goes; an old-name entry of another folder stays'
    Assert (-not (Test-Path -LiteralPath $machineDir)) 'uninstall: a folder left empty is removed (a set-aside folder of an interrupted upgrade is ours too)'
    Assert (-not (Test-Path -LiteralPath $tm.ActiveSetupKey)) 'uninstall (all users): the Active Setup component goes (no user gets icons of a removed copy)'
    Assert (-not (Test-Path (Join-Path $user1Desk 'Agentmaster.lnk'))) 'uninstall (all users): the icons it gave each user go'
    Assert ((Get-TestShortcut (Join-Path $user2Progs 'Agentmaster.lnk')).Target -like '*\users\two\own-copy\Agentmaster.exe') 'uninstall (all users): a user''s icon of their OWN install (the same name) stays'
    Assert (Test-Path (Join-Path $tm.Desktop 'Agentmaster.lnk')) 'uninstall: a shortcut leading to another copy stays'
    # An Active Setup component that starts another copy - a sibling folder whose name only begins like this one's
    # included - is not this copy's to remove.
    $tas = New-TestTargets 'active-setup-other' -Machine
    $siblingDir = Join-Path $root 'machine-install-2'
    New-Item -Path $tas.ActiveSetupKey -Force | Out-Null
    New-ItemProperty -LiteralPath $tas.ActiveSetupKey -Name 'StubPath' -Value ('"' + (Join-Path $siblingDir 'Agentmaster.exe') + '" --register-user') -PropertyType String -Force | Out-Null
    $null = Unregister-AmInstall -Scope Machine -Dir (Join-Path $root 'machine-install') -Targets $tas
    Assert (Test-Path -LiteralPath $tas.ActiveSetupKey) 'uninstall (all users): an Active Setup component that starts another copy (a sibling folder) stays'
    New-ItemProperty -LiteralPath $tas.ActiveSetupKey -Name 'StubPath' -Value ((Join-Path $siblingDir 'Agentmaster.exe') + ' --register-user') -PropertyType String -Force | Out-Null
    $null = Unregister-AmInstall -Scope Machine -Dir $siblingDir -Targets $tas
    Assert (-not (Test-Path -LiteralPath $tas.ActiveSetupKey)) 'uninstall (all users): ...and goes with the copy it starts (an unquoted command too)'

    # ---- a user's own install removed while the all-users one stays: its icons and pins go back to that copy -------
    $sharedDir = Join-Path $root 'shared-all-users'
    New-TestLayout -Dir $sharedDir
    Set-AmInstallDecision -Dir $sharedDir -Value 'machine'
    $th = New-TestTargets 'handback'
    $th.MachineDir = $sharedDir
    New-Item -Path $th.ActiveSetupKey -Force | Out-Null
    New-ItemProperty -LiteralPath $th.ActiveSetupKey -Name 'Version' -Value '1,2026,1022,1200' -PropertyType String -Force | Out-Null
    $ownDir = Join-Path $root 'own-install'
    $null = Install-AmUserCopy -LayoutDir $src -DestDir $ownDir -Version '9.9.9.0' -Targets $th
    New-TestShortcut -Lnk (Join-Path $th.PinsDir 'Agentmaster.lnk') -Target (Join-Path $ownDir 'Agentmaster.exe') -Arguments '--pinned'
    New-TestShortcut -Lnk (Join-Path $th.PinsDir 'Other app.lnk') -Target (Join-Path $root 'other-app\Other.exe')
    $null = Uninstall-AmCopy -Scope User -Dir $ownDir -Targets $th
    $sharedExe = Join-Path $sharedDir 'Agentmaster.exe'
    Assert ((Get-TestShortcut (Join-Path $th.Desktop 'Agentmaster.lnk')).Target -eq $sharedExe -and (Get-TestShortcut (Join-Path $th.StartMenu 'Agentmaster.lnk')).Target -eq $sharedExe) 'hand back: the user''s desktop and Start-menu icons now open the all-users copy (the user is not left without one)'
    $pin = Get-TestShortcut (Join-Path $th.PinsDir 'Agentmaster.lnk')
    Assert ($pin.Target -eq $sharedExe -and $pin.Arguments -eq '--pinned') 'hand back: the taskbar pin is re-pointed in place (the same file: the taskbar keeps it)'
    Assert ((Get-TestShortcut (Join-Path $th.PinsDir 'Other app.lnk')).Target -like '*\other-app\Other.exe') 'hand back: another app''s pin is left alone'
    Assert ([string](Get-Item -LiteralPath $th.UserActiveSetupKey).GetValue('Version') -eq '1,2026,1022,1200') 'hand back: the all-users copy''s per-user step is marked done for this user (no second set of icons at the next sign-in)'
    Assert (-not (Test-Path -LiteralPath $th.UninstallKey) -and -not (Test-Path (Join-Path $ownDir 'Agentmaster.exe'))) 'hand back: the user''s own install is still removed'
    # The handed-back links get the shared copy's id from its own --register-user (Invoke-AmMachineCopyUserCommand) -
    # run only when the Active Setup component names that very exe (an older copy would start the app instead). A copy
    # of PING.EXE stands in for the shared exe: it ends at once whatever its arguments.
    $cmdDir = Join-Path $root 'stub-copy'
    New-Item -ItemType Directory -Force -Path $cmdDir | Out-Null
    Copy-Item -LiteralPath "$env:SystemRoot\System32\PING.EXE" -Destination (Join-Path $cmdDir 'Agentmaster.exe')
    $tc = New-TestTargets 'stub-command' -Machine
    Assert (-not (Invoke-AmMachineCopyUserCommand -MachineDir $cmdDir -Targets $tc)) 'user command: no Active Setup component -> not run'
    New-Item -Path $tc.ActiveSetupKey -Force | Out-Null
    New-ItemProperty -LiteralPath $tc.ActiveSetupKey -Name 'StubPath' -Value ('"' + (Join-Path $root 'another-copy\Agentmaster.exe') + '" --register-user') -PropertyType String -Force | Out-Null
    Assert (-not (Invoke-AmMachineCopyUserCommand -MachineDir $cmdDir -Targets $tc)) 'user command: a component that starts another copy -> not run'
    New-ItemProperty -LiteralPath $tc.ActiveSetupKey -Name 'StubPath' -Value ('"' + (Join-Path $cmdDir 'Agentmaster.exe') + '" --register-user') -PropertyType String -Force | Out-Null
    Assert (Invoke-AmMachineCopyUserCommand -MachineDir $cmdDir -Targets $tc) 'user command: the component names this exe -> it runs, and is waited for'
    Assert (-not (Invoke-AmMachineCopyUserCommand -MachineDir (Join-Path $root 'no-copy-here') -Targets $tc)) 'user command: no exe in the folder -> not run'
    $tn = New-TestTargets 'no-shared'
    $ownDir2 = Join-Path $root 'own-install-2'
    $null = Install-AmUserCopy -LayoutDir $src -DestDir $ownDir2 -Version '9.9.9.0' -Targets $tn
    $null = Uninstall-AmCopy -Scope User -Dir $ownDir2 -Targets $tn
    Assert (-not (Test-Path (Join-Path $tn.Desktop 'Agentmaster.lnk')) -and -not (Test-Path -LiteralPath $tn.UserActiveSetupKey)) 'hand back: with no all-users copy the icons just go'

    # ---- the in-app updater's question when the all-users copy cannot be updated (am-update.ps1) -----------
    $declined = Get-MachineUpdateChoices -Code 1223
    Assert ($declined.Answers['a'] -eq 'admin' -and $declined.Answers['u'] -eq 'user' -and $declined.Answers['c'] -eq 'cancel') 'update choice (declined): ask again, just for you, cancel'
    Assert ($declined.Default -eq 'user') 'update choice: the default is the per-user install (no administrator rights, the same data)'
    $refused = Get-MachineUpdateChoices -Code 1260
    Assert (-not $refused.Answers.ContainsKey('a') -and $refused.Answers['u'] -eq 'user') 'update choice (refused by policy): asking again is not offered (it cannot help)'
    Assert ($refused.Reason -match 'does not let this account') 'update choice (refused by policy): the reason says so'
    $failed = Get-MachineUpdateChoices -Code 1
    Assert ($failed.Answers['a'] -eq 'admin' -and ($failed.Lines -join ' ') -match 'other users') 'update choice (the swap failed): asking again names the usual cause (other users still run it)'
    Assert (($failed.Lines -join ' ') -match 'starts again') 'update choice: cancel says the installed version starts again'
    Assert ($script:AmIsUpdateRun -eq $true -and $script:AmAppStarted -eq $false) 'updater run state: an update run that has started nothing yet'
    # Restart-PreviousApp's "already running again" check counts THIS account's processes only (another user's
    # instance of an all-users copy is visible to it, and counting it kept the user's app from starting again -
    # found in the test desktop, where that half is checked). A copy of ping.exe named Agentmaster.exe, run from
    # a scratch folder for a few seconds, is this account's.
    $fakeDir = Join-Path $root 'running-copy'
    New-Item -ItemType Directory -Force -Path $fakeDir | Out-Null
    Copy-Item -LiteralPath "$env:SystemRoot\System32\PING.EXE" -Destination (Join-Path $fakeDir 'Agentmaster.exe')
    $fake = Start-Process -FilePath (Join-Path $fakeDir 'Agentmaster.exe') -ArgumentList '-n', '30', '127.0.0.1' -WindowStyle Hidden -PassThru
    try {
        Start-Sleep -Milliseconds 800
        Assert (Test-AmAppRunningUnder -Dir $fakeDir) 'running check: this account''s Agentmaster.exe from the folder counts'
        Assert (-not (Test-AmAppRunningUnder -Dir $fakeDir -ExceptPid $fake.Id)) 'running check: the excepted pid (the closing app) does not'
        Assert (-not (Test-AmAppRunningUnder -Dir (Join-Path $root 'nothing-runs-here'))) 'running check: another folder -> not running'
    } finally {
        Stop-Process -Id $fake.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $fake.Id -Timeout 5 -ErrorAction SilentlyContinue
    }
    # The marker the restarted app reads so it does not offer the same update again at once (Updater.h
    # ConsumeRestartedAfterUpdateMarker): two lines, the tag and the UTC write time in Unix milliseconds.
    $markerDir = Join-Path $root 'profile-for-marker'
    New-Item -ItemType Directory -Force -Path $markerDir | Out-Null
    $before = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    Write-RestartedMarker -Dir $markerDir
    $markerLines = @([IO.File]::ReadAllText((Join-Path $markerDir 'am-update-restarted.txt')) -split "`n" | Where-Object { $_ })
    Assert ($markerLines.Count -eq 2 -and $markerLines[0] -eq 'unknown' -and [long]$markerLines[1] -ge $before -and [long]$markerLines[1] -le $before + 60000) 'restart marker: the tag (unknown without -Version) and the write time'
    Assert (@([IO.File]::ReadAllBytes((Join-Path $markerDir 'am-update-restarted.txt')))[0] -ne 0xEF) 'restart marker: no BOM (the app reads plain UTF-8; it strips a BOM too)'
    Write-RestartedMarker -Dir ''
    Write-RestartedMarker -Dir (Join-Path $root 'no-such-profile')
    Assert (-not (Test-Path (Join-Path $root 'no-such-profile'))) 'restart marker: no profile folder -> nothing written, nothing created'

    # ---- the identity and the phase machinery ---------------------------------------------------------------
    $env:USERPROFILE = $savedUserProfile
    $id = Get-AmIdentity
    Assert ($id.RunningSid -eq [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) 'identity: the running account'
    Assert ($id.IsMainUser) 'identity: the test runs as the signed-in user'
    $phaseDir = Join-Path $root 'phase'
    New-Item -ItemType Directory -Force -Path $phaseDir | Out-Null
    $ps1 = Write-AmPhaseScript -PhaseDir $phaseDir -Body "Write-Ok 'phase ran'; [IO.File]::WriteAllText((Join-Path `$PhaseDir 'result.txt'), (Get-AmDefaultUserInstallDir))"
    $tokens = $null
    $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($ps1, [ref]$tokens, [ref]$errors)
    Assert ($errors.Count -eq 0) 'phase: the generated phase script parses'
    $p = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ps1`"" -Wait -PassThru -WindowStyle Hidden
    Assert ($p.ExitCode -eq 0) "phase: it runs with the core loaded (exit $($p.ExitCode))"
    $result = Read-AmPhaseRecord -PhaseDir $phaseDir
    Assert ($result -eq (Get-AmDefaultUserInstallDir)) 'phase: its result comes back'
    # An all-users install registers itself inside an elevated phase: the Active Setup id must be defined there.
    $asDir = Join-Path $root 'phase-active-setup'
    New-Item -ItemType Directory -Force -Path $asDir | Out-Null
    $ps1a = Write-AmPhaseScript -PhaseDir $asDir -Body "[IO.File]::WriteAllText((Join-Path `$PhaseDir 'result.txt'), (Get-AmRegistrationTargets -Scope Machine).ActiveSetupKey)"
    $pa = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ps1a`"" -Wait -PassThru -WindowStyle Hidden
    Assert ($pa.ExitCode -eq 0 -and (Read-AmPhaseRecord -PhaseDir $asDir) -eq $real.ActiveSetupKey) 'phase: the Active Setup id is defined in a phase script (an all-users registration runs in one)'
    $failDir = Join-Path $root 'phase-fail'
    New-Item -ItemType Directory -Force -Path $failDir | Out-Null
    $ps1f = Write-AmPhaseScript -PhaseDir $failDir -Body "throw 'planned failure'"
    $pf = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ps1f`"" -Wait -PassThru -WindowStyle Hidden
    Assert ($pf.ExitCode -eq 1) 'phase: a failing body exits 1'

    if ($IncludeScheduledTask) {
        $code = Invoke-AmAsUser -UserSid $id.RunningSid -FilePath "$env:SystemRoot\System32\cmd.exe" -Arguments '/c exit 7' -Wait -TimeoutSec 120
        Assert ($code -eq 7) "scheduled task: runs as the user and reports the exit code (got $code)"
    }
}
finally {
    $env:USERPROFILE = $savedUserProfile
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $regRoot) { Remove-Item -LiteralPath $regRoot -Recurse -Force -ErrorAction SilentlyContinue }
    $env:AGENTMASTER_INSTALLER_LIBRARY = $null
}

Write-Host ("{0} checks, {1} failures" -f $script:checks, $script:failures)
exit $script:failures
