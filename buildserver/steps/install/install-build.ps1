<#
.SYNOPSIS
    Installs a build server build on one of this PC's Agentmaster instances (dev or production): the Windows side of
    `amb install` (the coordinator's deploy job runs it through WSL's interop).

.DESCRIPTION
    What session 14fdd99b did by hand for dev 0.0.1.36-0.0.1.39 and production 0.8.0.1 (2026-10-26/28), one checked
    step at a time. Nothing is stopped before step 9; a failure before it leaves the instance and its slots exactly as
    they were.
      1. The registration: the instance must be a LOOSE registration (an installed MSIX needs the swap's
         -ReplaceInstalledPackage and a rollback bundle: a person's decision, CLAUDE.md "Hot deploy") running from one
         folder of its pair; the build goes into the OTHER one.
     1b. The same build: when the running TerminalApp.dll is the build's, byte for byte, the instance already runs this
         build. Nothing is staged or stopped and the run succeeds (unless -Reinstall): a swap onto it would only
         restart the instance and cut off every running turn.
      2. That folder must not run anything; the zip's SHA-256 must be the store's.
      3. Stage: an older build in that folder is moved aside (<slot>.amb-prev-<stamp>), the zip extracted into a fresh
         folder (.NET's ZipFile, which refuses an entry that would land outside it).
      4. Check: the manifest names the instance's package at a build's 0.0.1.0, the key binaries are there,
         TerminalApp.dll bakes the build's commit (the build-info header stores `git rev-parse --short=9 HEAD` as
         UTF-16LE text), and no DLL or EXE the running layout has is missing (the app could not load it).
      5. -DryRun: the swap's own dry run (-WhatIf), then the slot is put back. Ends here.
      6. The requesting session: when -RequestedByPid is one of the instance's sessions, wait until it has not been
         Running for 15 s: its final message must be written before the swap ends its claude.
      7. The other sessions: with -WaitOthersMinutes 0 (the owner's choice of 2026-10-20: "cut off at once") the swap
         does not wait for them; their running turns are named, and the new build's restore wake starts them again.
      8. The build-launch mutex (tools\am-lock.sh, the committed copy the coordinator staged): every build, launch and
         swap on this PC takes it.
      9. The swap: tools\Swap-AgentmasterLayout.ps1 (the committed copy), run in this process so its result object
         comes back: stamp, stop the GUI only, drain, register, launch; on any failure after the stop it brings the old
         version back itself and throws. The staged slot is then put back too.
     10. The watch: the new GUI must still run 60 s later; a "Reopen your N previous Agentmaster windows?" dialog (it
         holds the restore until someone answers) and the instance's new trouble lines in hooks.log are reported.
    Every line goes to standard output (the coordinator streams it as the request's progress); the last one is
    "INSTALL {json}", also written to -Result.

.PARAMETER Instance
    "dev" or "production".

.PARAMETER Package
    The instance's package: AgentmasterDev or Agentmaster.

.PARAMETER CliInstance
    The instance's name for agentmaster-cli's --instance: dev or release.

.PARAMETER Slots
    The instance's two layout folders, ';'-separated (one string: `pwsh -File` passes "a,b" as one string, never as an
    array).

.PARAMETER Zip
    The build's layout zip, staged by the coordinator.

.PARAMETER Sha256
    The zip's SHA-256 in the build's manifest.

.PARAMETER BuildId
    The build id (for the log and the lock's label).

.PARAMETER Commit
    The build's commit (a full SHA): TerminalApp.dll must bake its first 9 characters.

.PARAMETER SwapScript
    The staged tools\Swap-AgentmasterLayout.ps1.

.PARAMETER LockScript
    The staged tools\am-lock.sh.

.PARAMETER RequestId
    The coordinator's request (for the lock's label).

.PARAMETER RequestedByPid
    The claude.exe the request came from, or 0.

.PARAMETER WaitOthersMinutes
    How long to wait for the instance's other running sessions; 0 = none.

.PARAMETER OwnWaitMinutes
    How long to wait for the requesting session's turn to end before giving up (nothing changed then).

.PARAMETER WatchSeconds
    How long to watch the new instance after the swap.

.PARAMETER DryRun
    Stage, check and run the swap's -WhatIf, then put the slot back.

.PARAMETER Reinstall
    Install even when the instance already runs this very build (step 1b): to repair a damaged layout or registration.
    Without it such a run changes nothing and succeeds.

.PARAMETER InjectFailureAfterStop
    A test: pass -InjectFailureAfterStop to the swap, which must then roll back.

.PARAMETER Result
    Where the result JSON goes.

.PARAMETER CancelFile
    When this file appears, the script stops at its next safe point (any wait before the swap); the swap itself is
    never interrupted.

.OUTPUTS
    Progress lines, then "INSTALL {json}" (ok, dryRun, phase, the old and new version and folder, the downtime, the
    rollback, the cut-off turns, the watch's findings, a message).

.NOTES
    PowerShell 7 (the swap script requires it). Exit codes: 0 installed (or the dry run passed, or the instance already
    runs this build: nothing done, the result's alreadyInstalled is true); 1 refused or failed
    before anything was stopped (the instance and its slots unchanged); 2 the swap failed after the stop and the old
    version came back; 3 the swap failed and its rollback did not finish, or the new instance is not running: look at
    the instance now; 4 cancelled before the swap.
#>
param(
    [Parameter(Mandatory)][ValidateSet('dev', 'production')][string]$Instance,
    [Parameter(Mandatory)][ValidateSet('AgentmasterDev', 'Agentmaster')][string]$Package,
    [Parameter(Mandatory)][ValidateSet('dev', 'release')][string]$CliInstance,
    [Parameter(Mandatory)][string]$Slots,
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$Sha256,
    [Parameter(Mandatory)][string]$BuildId,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$SwapScript,
    [Parameter(Mandatory)][string]$LockScript,
    [Parameter(Mandatory)][string]$RequestId,
    [int]$RequestedByPid = 0,
    [ValidateRange(0, 240)][int]$WaitOthersMinutes = 0,
    [ValidateRange(1, 240)][int]$OwnWaitMinutes = 45,
    [ValidateRange(0, 600)][int]$WatchSeconds = 60,
    [switch]$DryRun,
    [switch]$Reinstall,
    [switch]$InjectFailureAfterStop,
    [Parameter(Mandatory)][string]$Result,
    [Parameter(Mandatory)][string]$CancelFile
)
$ErrorActionPreference = 'Stop'
# A native command's non-zero exit code must never throw here: each native call reads $LASTEXITCODE itself.
$PSNativeCommandUseErrorActionPreference = $false
$gitBash = 'C:\Program Files\Git\bin\bash.exe'
$short = $Commit.Substring(0, [Math]::Min(9, $Commit.Length)).ToLowerInvariant()

# What the result says; filled step by step, written once at the end.
$facts = [ordered]@{
    ok = $false; dryRun = [bool]$DryRun; phase = 'check'; instance = $Instance; package = $Package
    buildId = $BuildId; commit = $Commit; slot = $null
    fromVersion = $null; fromLocation = $null; toVersion = $null; toLocation = $null
    downtimeSeconds = $null; newPid = $null; rolledBack = $false; cutOff = @(); asking = @()
    waitedSeconds = 0; alive = $null; reopenDialog = $false; trouble = @(); swapScript = $SwapScript; message = ''
    alreadyInstalled = $false
}
$exitCode = 1
$token = $null
# The staging state, for the put-back (Restore-Slot): the folder the build went into, and where its old contents went.
$script:slot = $null
$script:aside = $null
$script:extracted = $false

<#
.SYNOPSIS
    Prints one timestamped progress line.
.PARAMETER Text
    The line.
#>
function Say([string]$Text) { Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $Text) }

<#
.SYNOPSIS
    Ends the run: records the phase and message, and leaves through the finally block with an exit code.
.PARAMETER Code
    The exit code (see .NOTES).
.PARAMETER Message
    What happened, for the request's outcome.
#>
function Stop-Run([int]$Code, [string]$Message) {
    $script:exitCode = $Code
    $facts.message = $Message
    Say $Message
    throw [System.OperationCanceledException]::new('stop-run')
}

<#
.SYNOPSIS
    Throws Stop-Run's cancellation when the coordinator asked to cancel (the cancel file exists).
#>
function Test-Cancel {
    if (Test-Path -LiteralPath $CancelFile) { Stop-Run 4 'cancelled before the swap: nothing was stopped' }
}

<#
.SYNOPSIS
    Lists a folder's files as paths relative to it.
.PARAMETER Root
    The folder.
.OUTPUTS
    [System.Collections.Generic.HashSet[string]] the relative paths ('\' separators, case-insensitive).
#>
function Get-RelativeFileSet([string]$Root) {
    $set = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $prefix = $Root.TrimEnd('\').Length + 1
    foreach ($f in Get-ChildItem -LiteralPath $Root -Recurse -File -Force) { [void]$set.Add($f.FullName.Substring($prefix)) }
    return , $set
}

<#
.SYNOPSIS
    The processes whose executable lies under a folder.
.PARAMETER Folder
    The folder.
.OUTPUTS
    Win32_Process objects.
#>
function Get-ProcessUnder([string]$Folder) {
    $prefix = $Folder.TrimEnd('\') + '\'
    @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
}

<#
.SYNOPSIS
    Puts the staged slot back as it was before this run: the extracted build goes, the moved-aside folder comes back.
.DESCRIPTION
    Used whenever the build did not become the instance's (a failed check, a dry run, a cancel, a swap that rolled
    back). Never throws: a folder that cannot be put back is reported (the instance itself never runs from it).
#>
function Restore-Slot {
    if (-not $script:slot) { return }
    try {
        # Never the folder the instance runs from, whatever this script believes: a swap that registered the build but
        # whose result was lost would otherwise have its live layout deleted here.
        $live = Get-AppxPackage -Name $Package | Select-Object -First 1
        if ($live -and $live.InstallLocation.TrimEnd('\') -ieq $script:slot) {
            Say "NOT putting $($script:slot) back: $Package runs from it now"
            $script:extracted = $false
            return
        }
        $users = @(Get-ProcessUnder $script:slot)
        if ($users.Count) {
            Say ("NOT putting {0} back: processes run from it ({1})" -f $script:slot, (($users | ForEach-Object { "$($_.Name)#$($_.ProcessId)" }) -join ', '))
            return
        }
        if ($script:extracted -and (Test-Path -LiteralPath $script:slot)) { Remove-Item -LiteralPath $script:slot -Recurse -Force }
        if ($script:aside -and (Test-Path -LiteralPath $script:aside)) {
            Rename-Item -LiteralPath $script:aside -NewName (Split-Path $script:slot -Leaf)
            Say "put $($script:slot) back as it was"
        }
        elseif ($script:extracted) { Say "removed the staged build from $($script:slot)" }
    }
    catch { Say "COULD NOT PUT $($script:slot) BACK ($($_.Exception.Message)); its earlier contents are in $($script:aside)" }
    $script:extracted = $false
}

<#
.SYNOPSIS
    Whether the instance already runs this very build: its running TerminalApp.dll is the build's, byte for byte.
.DESCRIPTION
    Every build links TerminalApp.dll anew and bakes its commit into it, so two builds never share it, while the same
    build always does (it comes from the same zip). The bytes are compared, never the baked commit: a local build of a
    dirty tree bakes HEAD's commit too, and such a layout must still be replaced. A different length settles it without
    reading either file. Never throws: anything that cannot be read counts as "not the same build", and the install goes
    on through its own checks (a damaged zip is refused by step 2's SHA-256).
.PARAMETER Running
    The layout folder the instance runs from.
.PARAMETER Zip
    The build's layout zip (the layout at its root).
.OUTPUTS
    [bool] $true only when both files were read and are identical.
#>
function Test-RunsThisBuild([string]$Running, [string]$Zip) {
    $live = Join-Path $Running 'TerminalApp.dll'
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
        try {
            # By the name the extraction gives it, never GetEntry's exact one: the build VM's archiver writes every entry
            # as "./<path>" (seen in b20261104-1's layout zip), and an archive may use '\' and another case.
            $entry = $archive.Entries | Where-Object { (($_.FullName -replace '\\', '/') -replace '^(\./)+', '') -ieq 'TerminalApp.dll' } |
                Select-Object -First 1
            if (-not $entry) {
                Say "the build's zip holds no TerminalApp.dll at its root: not compared with the running layout"
                return $false
            }
            $liveFile = Get-Item -LiteralPath $live -ErrorAction Stop
            if ($entry.Length -ne $liveFile.Length) { return $false }
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $buildStream = $entry.Open()
                try { $buildHash = [Convert]::ToHexString($sha.ComputeHash($buildStream)) } finally { $buildStream.Dispose() }
                # The running app maps this DLL as an image: read it while sharing everything, so the loader's hold and
                # the app itself are never disturbed.
                $liveStream = [IO.File]::Open($live, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                try { $liveHash = [Convert]::ToHexString($sha.ComputeHash($liveStream)) } finally { $liveStream.Dispose() }
            }
            finally { $sha.Dispose() }
            return $buildHash -eq $liveHash
        }
        finally { $archive.Dispose() }
    }
    catch {
        Say "could not compare the running TerminalApp.dll with the build's ($($_.Exception.Message)): installing"
        return $false
    }
}

<#
.SYNOPSIS
    Reads the instance's open sessions through the instance's own console CLI.
.DESCRIPTION
    The agentmaster-cli.exe of the layout the instance runs from, by full path, with --instance (never a bare
    "agentmaster": that name can start a second GUI). Never throws: a failure comes back as Ok = $false, which every
    caller treats as "not known to be at rest".
.PARAMETER Folder
    The layout the instance runs from.
.OUTPUTS
    [pscustomobject] with Ok, Rows (every open session: id, title, pid, derivedState) and Detail.
#>
function Get-Sessions([string]$Folder) {
    $ErrorActionPreference = 'Continue'
    try {
        $cli = Join-Path $Folder 'agentmaster-cli.exe'
        $json = (& $cli sessions --json --instance $CliInstance 2>$null) -join "`n"
        if ($LASTEXITCODE -ne 0 -or -not $json) { return [pscustomobject]@{ Ok = $false; Rows = @(); Detail = "the CLI exited $LASTEXITCODE" } }
        return [pscustomobject]@{ Ok = $true; Rows = @(($json | ConvertFrom-Json).sessions); Detail = '' }
    }
    catch { return [pscustomobject]@{ Ok = $false; Rows = @(); Detail = $_.Exception.Message } }
}

<#
.SYNOPSIS
    Formats session rows as 'id8 "title"'.
.PARAMETER Rows
    The rows.
.OUTPUTS
    [string[]] one entry per row.
#>
function Format-Rows($Rows) { @($Rows | ForEach-Object { '{0} "{1}"' -f "$($_.id)".PadRight(8).Substring(0, 8), $_.title }) }

<#
.SYNOPSIS
    Waits until the chosen sessions have not been Running, continuously, for HoldSec seconds.
.PARAMETER Folder
    The layout the instance runs from (its CLI).
.PARAMETER TimeoutSec
    The longest wait.
.PARAMETER HoldSec
    How long the rest must last.
.PARAMETER SessionPid
    Only the session whose claude has this process id; 0 = every session but -Except's. (Not "-Pid": PowerShell's
    read-only $PID would refuse the parameter.)
.PARAMETER Except
    A process id left out of the "every session" wait (the requesting session, already waited for).
.OUTPUTS
    [pscustomobject] with Ok (the rest held) and Running (the rows still running when the time ran out).
#>
function Wait-AtRest([string]$Folder, [int]$TimeoutSec, [int]$HoldSec, [int]$SessionPid = 0, [int]$Except = 0) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $quietSince = $null
    $last = ''
    $running = @()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        Test-Cancel
        $s = Get-Sessions $Folder
        if (-not $s.Ok) { $quietSince = $null; $msg = "cannot read the sessions ($($s.Detail)) - waiting" }
        else {
            $running = @($s.Rows | Where-Object { $_.derivedState -eq 'Running' })
            if ($SessionPid) { $running = @($running | Where-Object { $_.pid -eq $SessionPid }) }
            elseif ($Except) { $running = @($running | Where-Object { $_.pid -ne $Except }) }
            if ($running.Count) { $quietSince = $null; $msg = 'waiting for: ' + ((Format-Rows $running) -join ', ') }
            else {
                if (-not $quietSince) { $quietSince = Get-Date }
                if (((Get-Date) - $quietSince).TotalSeconds -ge $HoldSec) { return [pscustomobject]@{ Ok = $true; Running = @() } }
                $msg = 'at rest - holding'
            }
        }
        if ($msg -ne $last) { Say $msg; $last = $msg }
        Start-Sleep -Seconds 3
    }
    return [pscustomobject]@{ Ok = $false; Running = $running }
}

<#
.SYNOPSIS
    The instance's profile folder: the one its package family names in %USERPROFILE%\.agentmaster.profiles, else the
    identity's default (~\.agentmaster-dev for dev, ~\.agentmaster for production).
.OUTPUTS
    [string] the folder.
#>
function Get-ProfileDir {
    $default = Join-Path $env:USERPROFILE $(if ($Package -eq 'AgentmasterDev') { '.agentmaster-dev' } else { '.agentmaster' })
    $map = Join-Path $env:USERPROFILE '.agentmaster.profiles'
    try {
        foreach ($line in Get-Content -LiteralPath $map -ErrorAction Stop) {
            if ($line -match "^$([regex]::Escape($Package))_[a-z0-9]+=(.+)$") { return $Matches[1].Trim() }
        }
    }
    catch { }
    return $default
}

# The process's top-level windows, to see the "Reopen your N previous Agentmaster windows?" TaskDialog (class #32770).
Add-Type -Namespace AmbInstall -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, System.IntPtr lp);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr lp);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
'@ -ErrorAction SilentlyContinue

<#
.SYNOPSIS
    Whether a process shows a visible dialog box (window class #32770).
.PARAMETER ProcessId
    The process.
.OUTPUTS
    [bool]; $false when the enumeration fails.
#>
function Test-DialogUp([int]$ProcessId) {
    # Script scope: EnumWindows calls the delegate, and only the script scope is certain to be visible from it.
    $script:dialogSeen = $false
    $script:dialogPid = $ProcessId
    $cb = [AmbInstall.Win+EnumProc] {
        param($h, $lp)
        $owner = 0
        [void][AmbInstall.Win]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $script:dialogPid -and [AmbInstall.Win]::IsWindowVisible($h)) {
            $c = New-Object System.Text.StringBuilder 64
            [void][AmbInstall.Win]::GetClassName($h, $c, 64)
            if ($c.ToString() -eq '#32770') { $script:dialogSeen = $true }
        }
        return $true
    }
    try { [void][AmbInstall.Win]::EnumWindows($cb, [IntPtr]::Zero) } catch { }
    return $script:dialogSeen
}

try {
    Say "install $BuildId ($short) on $Instance ($Package)$(if ($DryRun) { ' - DRY RUN' })$(if ($InjectFailureAfterStop) { ' - with an INJECTED FAILURE after the stop (a test)' })"

    # 1. The registration and the target folder.
    $reg = Get-AppxPackage -Name $Package | Select-Object -First 1
    if (-not $reg) { Stop-Run 1 "$Package is not registered on this PC: its first install is a person's job (CLAUDE.md `"Deploy & run`"). Nothing changed." }
    $running = $reg.InstallLocation.TrimEnd('\')
    $facts.fromVersion = "$($reg.Version)"
    $facts.fromLocation = $running
    Say "$Package now: $($reg.Version) @ $running"
    if ($reg.IsDevelopmentMode -eq $false) {
        Stop-Run 1 "$Package is an INSTALLED package ($($reg.PackageFullName)), not a loose registration: replacing it takes the swap's -ReplaceInstalledPackage and the installed version's bundle, a person's decision (CLAUDE.md `"Hot deploy`"). Nothing changed."
    }
    $pair = @($Slots.Split(';', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { $_.Trim().TrimEnd('\') })
    if ($pair.Count -ne 2) { Stop-Run 1 "expected two layout folders, not '$Slots'. Nothing changed." }
    if (-not ($pair | Where-Object { $_ -ieq $running })) {
        Stop-Run 1 "$Package runs from $running, which is not one of its folders ($($pair -join ', ')): install by hand. Nothing changed."
    }

    # 1b. The instance may run this very build already: a swap onto it would only restart the instance, ending every
    #     claude it hosts and cutting off their running turns, for nothing. 2026-11-04: a session inside production
    #     re-ran its install after every restart (the swap had cut its own turn off, and Claude Code continued the turn),
    #     and production restarted 6 times in 9 hours on one build. Before the stage, so nothing on disk moves either.
    if (Test-RunsThisBuild -Running $running -Zip $Zip) {
        if (-not $Reinstall) {
            $facts.phase = 'already'
            $facts.alreadyInstalled = $true
            $facts.ok = $true
            $exitCode = 0
            $facts.message = "$(if ($DryRun) { 'dry run: ' })$Package already runs $BuildId ($short): its TerminalApp.dll is the build's, byte for byte ($($reg.Version) @ $running). Nothing was stopped; amb install --reinstall installs it again."
            Say $facts.message
            return
        }
        Say "$Package already runs this build: installing it again (-Reinstall)"
    }
    $target = $pair | Where-Object { $_ -ine $running } | Select-Object -First 1
    $facts.slot = $target
    Say "the build goes into $target (the folder $Package does not run from)"

    # 2. The target runs nothing; the zip is the store's.
    $busy = @(Get-ProcessUnder $target)
    if ($busy.Count) { Stop-Run 1 ("processes run from {0}: {1}. Nothing changed." -f $target, (($busy | ForEach-Object { "$($_.Name)#$($_.ProcessId)" }) -join ', ')) }
    $hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $Sha256.ToLowerInvariant()) { Stop-Run 1 "the staged zip's SHA-256 is $hash, not the build's $Sha256. Nothing changed." }
    Say ("layout zip {0} ({1:N1} MB), SHA-256 checked" -f (Split-Path $Zip -Leaf), ((Get-Item -LiteralPath $Zip).Length / 1MB))

    # 3. Stage: the folder's older build moves aside (it comes back on any failure), the zip goes into a fresh one.
    $facts.phase = 'stage'
    $script:slot = $target
    if ((Test-Path -LiteralPath $target) -and (Get-ChildItem -LiteralPath $target -Force | Select-Object -First 1)) {
        $script:aside = '{0}.amb-prev-{1:yyyyMMdd-HHmmss}' -f $target, (Get-Date)
        Rename-Item -LiteralPath $target -NewName (Split-Path $script:aside -Leaf)
        Say "moved the folder's earlier contents aside to $($script:aside)"
    }
    elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force -Recurse }
    New-Item -ItemType Directory -Path $target | Out-Null
    $script:extracted = $true
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $target)
    $newSet = Get-RelativeFileSet $target
    Say ("extracted {0} files in {1:N1} s" -f $newSet.Count, $sw.Elapsed.TotalSeconds)

    # 4. Check the layout.
    $facts.phase = 'check'
    $fail = @()
    $manifestPath = Join-Path $target 'AppxManifest.xml'
    if (-not (Test-Path -LiteralPath $manifestPath)) { $fail += 'AppxManifest.xml is missing (the zip does not hold the layout at its root)' }
    else {
        [xml]$m = Get-Content -LiteralPath $manifestPath -Raw
        $id = $m.Package.Identity
        Say "manifest: $($id.Name) $($id.Version) ($($id.ProcessorArchitecture))"
        if ($id.Name -ne $Package) { $fail += "the manifest names $($id.Name), not $Package (a $(if ($Package -eq 'Agentmaster') { 'release' } else { 'dev' })-identity build is needed)" }
        if ($id.Version -ne '0.0.1.0') { $fail += "the manifest's version is $($id.Version), not a build's 0.0.1.0" }
    }
    foreach ($k in 'Agentmaster.exe', 'TerminalApp.dll', 'Microsoft.Terminal.Control.dll', 'agentmaster-cli.exe', 'OpenConsole.exe', 'resources.pri') {
        if (-not (Test-Path -LiteralPath (Join-Path $target $k))) { $fail += "$k is missing" }
    }
    $dll = Join-Path $target 'TerminalApp.dll'
    if (Test-Path -LiteralPath $dll) {
        if ([Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($dll)).Contains($short)) { Say "TerminalApp.dll bakes $short" }
        else { $fail += "TerminalApp.dll does not bake $short" }
    }
    $oldSet = Get-RelativeFileSet $running
    $onlyOld = @($oldSet | Where-Object { -not $newSet.Contains($_) } | Sort-Object)
    $onlyNew = @($newSet | Where-Object { -not $oldSet.Contains($_) } | Sort-Object)
    Say ("compared with {0}: {1} files there, {2} here; {3} only here, {4} only there" -f $running, $oldSet.Count, $newSet.Count, $onlyNew.Count, $onlyOld.Count)
    $onlyNew | Select-Object -First 10 | ForEach-Object { Say "  only in the build: $_" }
    $onlyOld | Select-Object -First 10 | ForEach-Object { Say "  only in the running layout: $_" }
    $missing = @($onlyOld | Where-Object { $_ -match '\.(dll|exe)$' })
    if ($missing.Count) { $fail += "the build lacks binaries the running layout has: $($missing -join ', ')" }
    if ($fail.Count) { Stop-Run 1 ('the layout failed its checks: ' + ($fail -join '; ') + '. Nothing was stopped; the folder is put back.') }
    Say 'the layout passed every check'
    Test-Cancel

    # 5. A dry run ends with the swap's own plan.
    if ($DryRun) {
        $facts.phase = 'dry-run'
        & $SwapScript -LayoutDir $target -PackageName $Package -WhatIf 6>&1 3>&1 | ForEach-Object { Say "  | $_" }
        # A dry run changes nothing: the staged build goes and the folder's earlier contents come back.
        Restore-Slot
        $exitCode = 0
        $facts.ok = $true
        $facts.message = "dry run: $BuildId would go into $target and $Package would move there from $running"
        Say $facts.message
        return
    }

    # 6. The requesting session's turn must end first (its final message is written before its claude is ended).
    $facts.phase = 'wait'
    $waitClock = [Diagnostics.Stopwatch]::StartNew()
    if ($RequestedByPid) {
        $s = Get-Sessions $running
        $mine = @($s.Rows | Where-Object { $_.pid -eq $RequestedByPid })
        if ($mine.Count) {
            Say ("waiting for session {0} (it asked for this install; claude pid {1}) to end its turn" -f (Format-Rows $mine)[0], $RequestedByPid)
            $own = Wait-AtRest -Folder $running -TimeoutSec ($OwnWaitMinutes * 60) -HoldSec 15 -SessionPid $RequestedByPid
            if (-not $own.Ok) { Stop-Run 1 "the requesting session stayed Running for $OwnWaitMinutes min: not swapped, nothing changed (the folder is put back)." }
            Say 'the requesting session is at rest'
        }
        elseif ($s.Ok) { Say "the requesting claude (pid $RequestedByPid) is not one of $($Instance)'s sessions: no wait for it" }
        else { Say "cannot read $($Instance)'s sessions ($($s.Detail)): no wait for the requesting session" }
    }

    # 7. The other sessions.
    if ($WaitOthersMinutes -gt 0) {
        $all = Wait-AtRest -Folder $running -TimeoutSec ($WaitOthersMinutes * 60) -HoldSec 10 -Except $RequestedByPid
        # @(...) around each call: a function's empty array comes back as $null, which the result would print as null.
        if (-not $all.Ok) { $facts.cutOff = @(Format-Rows $all.Running); Say ('still Running after {0} min - the swap cuts these turns off: {1}' -f $WaitOthersMinutes, ($facts.cutOff -join ', ')) }
    }
    else {
        $s = Get-Sessions $running
        if ($s.Ok) {
            $facts.cutOff = @(Format-Rows @($s.Rows | Where-Object { $_.derivedState -eq 'Running' -and $_.pid -ne $RequestedByPid }))
            $facts.asking = @(Format-Rows @($s.Rows | Where-Object { $_.derivedState -eq 'NeedsApproval' }))
            if ($facts.cutOff.Count) { Say ('the swap cuts these running turns off (no wait: the owner''s choice of 2026-10-20): ' + ($facts.cutOff -join ', ')) }
            else { Say "no other $Instance session is Running ($($s.Rows.Count) open)" }
            if ($facts.asking.Count) { Say ('these wait for an answer; the swap ends their claude too: ' + ($facts.asking -join ', ')) }
        }
        else { Say "cannot read $($Instance)'s sessions ($($s.Detail)): swapping without the list" }
    }
    $facts.waitedSeconds = [math]::Round($waitClock.Elapsed.TotalSeconds, 0)
    Test-Cancel

    # 8. The build-launch mutex.
    $facts.phase = 'lock'
    $lockPosix = '/' + $LockScript.Substring(0, 1).ToLowerInvariant() + $LockScript.Substring(2).Replace('\', '/')
    $label = "amb install $BuildId on $Instance ($RequestId)"
    $out = & $gitBash -c "bash '$lockPosix' acquire --wait 600 --label '$label'" 2>&1
    $lines = @($out | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { Stop-Run 1 ("could not take the build-launch mutex within 10 min ({0}). Nothing changed." -f (($lines | Select-Object -Last 2) -join ' ')) }
    $token = ($lines | Where-Object { $_ -notmatch '^\[am-lock\]' } | Select-Object -Last 1).Trim()
    Say 'took the build-launch mutex'
    # The lock can take 10 minutes; a cancel sent meanwhile must still stop this run before anything is stopped.
    Test-Cancel
    # Under the lock, the registration must still be the one checked: a swap by hand meanwhile would make this one wrong.
    $again = Get-AppxPackage -Name $Package | Select-Object -First 1
    if (-not $again -or $again.InstallLocation.TrimEnd('\') -ine $running -or "$($again.Version)" -ne $facts.fromVersion) {
        Stop-Run 1 "$Package changed while this install waited (now $($again.Version) @ $($again.InstallLocation)): not swapped, nothing changed."
    }

    # 9. The swap, in this process so its result object comes back; its lines (Write-Host: the information stream) and
    #    warnings are shown and kept. On a failure after the stop it has brought the old version back before throwing.
    $facts.phase = 'swap'
    Say "SWAP START: $Package $($facts.fromVersion) -> $target"
    $swapLines = [System.Collections.Generic.List[string]]::new()
    $swapResult = $null
    $swapArgs = @{ LayoutDir = $target; PackageName = $Package }
    if ($InjectFailureAfterStop) { $swapArgs.InjectFailureAfterStop = $true }
    try {
        & $SwapScript @swapArgs 6>&1 3>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.InformationRecord] -or $_ -is [System.Management.Automation.WarningRecord] -or $_ -is [string]) {
                $l = "$_"; $swapLines.Add($l); Write-Host "  | $l"
            }
            elseif ($_.PSObject.Properties['NewLocation']) { $swapResult = $_ }
            else { Write-Host "  | $_" }
        }
    }
    catch {
        $swapError = $_.Exception.Message
        $facts.rolledBack = [bool]($swapLines | Where-Object { $_ -match 'rollback: relaunching|rollback: reinstalled' })
        $rollbackFailed = [bool]($swapLines | Where-Object { $_ -match 'ROLLBACK FAILED|ROLLBACK: the relaunch failed' })
        $stopped = [bool]($swapLines | Where-Object { $_ -match 'stopped\s+GUI pid' })
        $now = Get-AppxPackage -Name $Package | Select-Object -First 1
        # The rollback's relaunch is an AUMID activation: its GUI appears a few seconds later (longer on a busy PC).
        $up = @()
        $upClock = [Diagnostics.Stopwatch]::StartNew()
        while (-not $up.Count -and $upClock.Elapsed.TotalSeconds -lt 45) {
            Start-Sleep -Seconds 1
            $up = @(Get-ProcessUnder $running | Where-Object { $_.Name -in 'Agentmaster.exe', 'WindowsTerminal.exe' })
        }
        Restore-Slot
        if (-not $stopped) { Stop-Run 1 "the swap refused before stopping anything: $swapError" }
        if ($facts.rolledBack -and -not $rollbackFailed -and $now -and $now.InstallLocation.TrimEnd('\') -ieq $running -and $up.Count) {
            Stop-Run 2 "the swap failed after the stop ($swapError) and brought $Package $($now.Version) back @ $running (pid $($up[0].ProcessId)); the build was not installed"
        }
        Stop-Run 3 "the swap failed after the stop ($swapError) and its rollback did not finish: $Package is $(if ($now) { "$($now.Version) @ $($now.InstallLocation)" } else { 'NOT registered' }), $(if ($up.Count) { 'running' } else { 'NOT running' }). Look at it now (CLAUDE.md `"The swap script`")."
    }
    # Whatever the result says, a build the instance runs from is never put back.
    $now = Get-AppxPackage -Name $Package | Select-Object -First 1
    if ($now -and $now.InstallLocation.TrimEnd('\') -ieq $target) { $script:extracted = $false }
    if (-not $swapResult) { Stop-Run 3 "the swap returned no result ($Package is $($now.Version) @ $($now.InstallLocation)): look at the instance now." }
    $facts.toVersion = "$($swapResult.NewVersion)"
    $facts.toLocation = "$($swapResult.NewLocation)"
    $facts.downtimeSeconds = $swapResult.DowntimeSeconds
    $facts.newPid = $swapResult.NewPid
    $script:extracted = $false   # the build is the instance's now: never put back
    Say ("installed: {0} {1} @ {2}, {3} s down" -f $Package, $facts.toVersion, $facts.toLocation, $facts.downtimeSeconds)

    # The older build moved aside is not needed any more: the instance's previous folder is the rollback target now.
    if ($script:aside -and (Test-Path -LiteralPath $script:aside)) {
        try { Remove-Item -LiteralPath $script:aside -Recurse -Force; Say "deleted the older build that was in $target" }
        catch { Say "could not delete $($script:aside) ($($_.Exception.Message)); delete it by hand" }
    }

    # 10. The watch: alive, the Reopen dialog, the new trouble lines.
    $facts.phase = 'watch'
    $hooksLog = Join-Path (Get-ProfileDir) 'hooks.log'
    $offset = 0L
    try { $offset = (Get-Item -LiteralPath $hooksLog).Length } catch { }
    $watchClock = [Diagnostics.Stopwatch]::StartNew()
    # The swap waits 60 s for the new GUI; a slower start leaves NewPid 0, so look for it under the new folder first.
    while (-not $swapResult.NewPid -and $watchClock.Elapsed.TotalSeconds -lt [Math]::Max(60, $WatchSeconds)) {
        Start-Sleep -Seconds 2
        $gui = Get-ProcessUnder $target | Where-Object { $_.Name -in 'Agentmaster.exe', 'WindowsTerminal.exe' } | Select-Object -First 1
        if ($gui) { $swapResult.NewPid = [int]$gui.ProcessId; $facts.newPid = $swapResult.NewPid; Say "the new GUI is pid $($gui.ProcessId)" }
    }
    $watchClock.Restart()
    $alive = [bool]$swapResult.NewPid
    while ($alive -and $watchClock.Elapsed.TotalSeconds -lt $WatchSeconds) {
        Start-Sleep -Seconds 2
        $alive = [bool](Get-Process -Id $swapResult.NewPid -ErrorAction SilentlyContinue)
        if (-not $facts.reopenDialog -and $alive -and (Test-DialogUp $swapResult.NewPid)) {
            $facts.reopenDialog = $true
            Say 'a dialog is up (likely "Reopen your N previous Agentmaster windows?"): the restore waits for someone to answer it'
        }
    }
    $facts.alive = $alive
    try {
        $fs = [IO.File]::Open($hooksLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try {
            # The lines the swap's new instance wrote: from the size before the watch (a log replaced meanwhile is read whole).
            if ($offset -gt $fs.Length) { $offset = 0 }
            $null = $fs.Seek($offset, [IO.SeekOrigin]::Begin)
            $text = (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd()
        }
        finally { $fs.Dispose() }
        $facts.trouble = @($text -split "`r?`n" | Where-Object {
                ($_ -match '\[exc\]' -and $_ -notmatch 'forensics installed') -or $_ -match '\[wil\]|\[launch-fail\]|\[persist-fail\]|\[startup-hang\]' -or
                ($_ -match '\[ui-stall\]' -and $_ -notmatch 'watchdog armed')
            } | Select-Object -First 10 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) })
    }
    catch { Say "could not read $hooksLog ($($_.Exception.Message))" }
    foreach ($t in $facts.trouble) { Say "  trouble: $t" }
    if (-not $alive) { Stop-Run 3 "installed, but the new $Package (pid $($swapResult.NewPid)) is NOT running $($watchClock.Elapsed.TotalSeconds.ToString('0')) s later: look at it now (crash dumps, the Application log)" }
    $facts.ok = $true
    $exitCode = 0
    $facts.phase = 'done'
    $facts.message = "installed $BuildId ($short) on $($Instance): $Package $($facts.toVersion) @ $($facts.toLocation), $($facts.downtimeSeconds) s down$(if ($facts.reopenDialog) { '; a dialog waits for an answer' })$(if ($facts.trouble.Count) { "; $($facts.trouble.Count) trouble line(s)" })"
    Say $facts.message
}
catch [System.OperationCanceledException] {
    # Stop-Run: the code and message are set; a run that never got as far as the swap's success puts the slot back.
    Restore-Slot
}
catch {
    $exitCode = if ($facts.phase -in 'swap', 'watch') { 3 } else { 1 }
    $facts.message = "unexpected error in phase $($facts.phase): $($_.Exception.Message)"
    Say $facts.message
    Restore-Slot
}
finally {
    if ($token) {
        $lockPosix = '/' + $LockScript.Substring(0, 1).ToLowerInvariant() + $LockScript.Substring(2).Replace('\', '/')
        & $gitBash -c "bash '$lockPosix' release --token '$token'" *> $null
        Say 'released the build-launch mutex'
    }
    $json = $facts | ConvertTo-Json -Compress -Depth 4
    try { Set-Content -LiteralPath $Result -Value $json -Encoding utf8NoBOM } catch { }
    Write-Host ('INSTALL ' + $json)
    exit $exitCode
}
