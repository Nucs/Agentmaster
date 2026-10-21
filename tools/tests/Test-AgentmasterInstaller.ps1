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
    the decision / pointer files; the write probe (a real deny ACL); the user and machine installs and their
    registration, written and removed; a phase script runs and reports back.

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
        param([string]$Name)
        @{
            UninstallKey = "$regRoot\$Name\Uninstall\Agentmaster"
            ClassesRoot  = "$regRoot\$Name\Classes"
            EnvKey       = "$regRoot\$Name\Environment"
            StartMenu    = Join-Path $root "$Name-startmenu"
            Desktop      = Join-Path $root "$Name-desktop"
            Broadcast    = $false
        }
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
    Install-AmUserCopy -LayoutDir $src -DestDir $userDir -Version '9.9.9.0' -Targets $tu
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
    $null = Register-AmInstall -Scope User -Dir $userDir -Version '9.9.9.0' -Targets $tu
    $path2 = (Get-Item -LiteralPath $tu.EnvKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    Assert ($path2 -eq $userDir) 'user install: a second registration does not add the PATH entry twice'
    $kept = Join-Path $root 'kept-portable'
    New-TestLayout -Dir $kept -Version '1.0.0.0'
    Set-AmInstallDecision -Dir $kept -Value 'portable'
    $tk = New-TestTargets 'kept'
    Install-AmUserCopy -LayoutDir $src -DestDir $kept -Version '9.9.9.0' -Targets $tk
    Assert ((Get-AmInstallDecision -Dir $kept) -eq 'portable' -and -not (Test-Path -LiteralPath $tk.UninstallKey)) 'user install: a folder kept portable by choice only gets its files updated'

    # ---- the machine install: refuses an old layout, writes machine, no pointer -----------------------------
    $tm = New-TestTargets 'machine'
    $oldLayout = Join-Path $root 'release-old'
    New-TestLayout -Dir $oldLayout -Version '0.7.9.0' -NoManifest
    $threw = $false
    try { Install-AmMachineCopy -LayoutDir $oldLayout -DestDir (Join-Path $root 'machine-old') -Version '0.7.9.0' -Targets $tm } catch { $threw = $true }
    Assert $threw 'machine install: a release without a manifest is refused (its app would not know install.path = machine)'
    $machineDir = Join-Path $root 'machine-install'
    Install-AmMachineCopy -LayoutDir $src -DestDir $machineDir -Version '9.9.9.0' -Targets $tm
    Assert ((Get-AmInstallDecision -Dir $machineDir) -eq 'machine') 'machine install: install.path = machine'
    Assert (-not (Test-Path (Join-Path $machineDir 'profile.path'))) 'machine install: no profile.path (each user resolves their own)'
    Assert ((Get-ItemProperty -LiteralPath $tm.UninstallKey).InstallLocation -eq $machineDir) 'machine install: registered'

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
    $null = Uninstall-AmCopy -Scope Machine -Dir $machineDir -Targets $tm
    Assert (-not (Test-Path -LiteralPath $machineDir)) 'uninstall: a folder left empty is removed'

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
