<#
.SYNOPSIS
    Measurement job: the compile benchmark once more with the balloon at 8 GB, sampling the guest's memory and CPU
    while it runs; then where the Build Tools install's space on C: went.

.DESCRIPTION
    - A runspace samples, every 2 s while compile-bench.ps1 runs: committed bytes, available memory, total CPU, and
      the processes taking the most CPU besides cl.exe / link.exe (what else competes in a fresh image).
      results\compile-guest-mp4-3.json (the benchmark) and results\compile-guest-mp4-3-samples.json (the samples).
    - Then the sizes of C:'s top-level folders and of the folders an installer leaves behind (the Windows Installer
      cache, ProgramData's Package Cache and Visual Studio folders, the TEMP folders), into results\c-sizes.json.

.OUTPUTS
    None; the JSON files on the share.
#>
$ErrorActionPreference = 'Continue'
$share = '\\host.lan\Data'
$tools = 'C:\bs\tools'

function Get-FolderSizeGB {
    <#
    .SYNOPSIS
        The total size of a folder's files in GB; $null when it does not exist.
    .PARAMETER Path
        The folder.
    .OUTPUTS
        [double] rounded to 2 places, or $null.
    #>
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    [math]::Round((Get-ChildItem $Path -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1GB, 2)
}

# The sampler: its own runspace, so the compile is timed by compile-bench.ps1 alone; it stops when the flag is set.
$state = [hashtable]::Synchronized(@{ Stop = $false; Samples = New-Object System.Collections.ArrayList })
# The sampler runs in its own runspace and gets the shared $state as its one argument ($args[0]); it appends a sample
# every 2 s until the main script sets $state.Stop.
$sampler = [PowerShell]::Create().AddScript({
    $state = $args[0]
    while (-not $state.Stop) {
        $c = Get-Counter '\Memory\Committed Bytes', '\Memory\Available MBytes', '\Processor(_Total)\% Processor Time' -ErrorAction SilentlyContinue
        $v = @($c.CounterSamples | ForEach-Object CookedValue)
        $others = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -notin 'cl', 'link', 'Idle' -and $_.CPU } |
            Sort-Object CPU -Descending | Select-Object -First 3 | ForEach-Object { "$($_.ProcessName) $([int]$_.CPU)s" }
        [void]$state.Samples.Add([pscustomobject][ordered]@{ t = (Get-Date).ToString('HH:mm:ss'); committedMB = [int]($v[0] / 1MB); availableMB = [int]$v[1]; cpu = [int]$v[2]; topCpuTotal = $others -join ', ' })
        Start-Sleep -Seconds 2
    }
}).AddArgument($state)
$handle = $sampler.BeginInvoke()
& "$tools\compile-bench.ps1" -SourceDir 'D:\m5src\src\cascadia\TerminalApp\AgentMaster\tests' `
    -VcVars 'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat' -Parallel 4 -OutFile "$share\results\compile-guest-mp4-3.json"
$state.Stop = $true
[void]$sampler.EndInvoke($handle)
$samples = @($state.Samples)
[ordered]@{
    peakCommittedMB  = ($samples | Measure-Object committedMB -Maximum).Maximum
    minAvailableMB   = ($samples | Measure-Object availableMB -Minimum).Minimum
    meanCpu          = [int](($samples | Measure-Object cpu -Average).Average)
    samples          = $samples
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$share\results\compile-guest-mp4-3-samples.json"

$top = Get-ChildItem C:\ -Directory -Force -ErrorAction SilentlyContinue | ForEach-Object {
    [ordered]@{ folder = $_.FullName; GB = Get-FolderSizeGB $_.FullName }
} | Sort-Object { $_.GB } -Descending
[ordered]@{
    freeGB          = [math]::Round((Get-PSDrive C).Free / 1GB, 2)
    topLevel        = @($top)
    WindowsInstaller = Get-FolderSizeGB "$env:windir\Installer"
    WinSxS          = Get-FolderSizeGB "$env:windir\WinSxS"
    PackageCache    = Get-FolderSizeGB "$env:ProgramData\Package Cache"
    ProgramDataVS   = Get-FolderSizeGB "$env:ProgramData\Microsoft\VisualStudio"
    UserTemp        = Get-FolderSizeGB $env:TEMP
    WindowsTemp     = Get-FolderSizeGB "$env:windir\Temp"
    pageFileMB      = @(Get-CimInstance Win32_PageFileUsage | ForEach-Object { "$($_.Name) $($_.AllocatedBaseSize)" })
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$share\results\c-sizes.json"
