<#
.SYNOPSIS
    Measurement job: the compile benchmark with Windows' servicing stopped, the fair guest number.

.DESCRIPTION
    After the Build Tools install, TiWorker.exe (the Windows Modules Installer worker, run by the TrustedInstaller
    service) kept one of the four vCPUs busy for over half an hour, and every compile run shared the CPU with it. This
    job stops the TrustedInstaller service and any TiWorker left, waits until the guest's CPU is idle (under 15 % for 10
    s, at most 3 minutes), records which processes still use CPU, then runs compile-bench.ps1 (run 4) while sampling
    CPU and memory every 2 s. results\compile-guest-mp4-4.json and results\compile-guest-mp4-4-samples.json.

.OUTPUTS
    None; the JSON files on the share.
#>
$ErrorActionPreference = 'Continue'
$share = '\\host.lan\Data'
$tools = 'C:\bs\tools'

function Get-Cpu {
    <#
    .SYNOPSIS
        The guest's total CPU use over one second, in percent.
    .OUTPUTS
        [int]
    #>
    [int](Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 1).CounterSamples.CookedValue
}

$before = @(Get-Process TiWorker -ErrorAction SilentlyContinue | ForEach-Object { "TiWorker pid $($_.Id) cpu $([int]$_.CPU)s" })
Stop-Service TrustedInstaller -Force -ErrorAction SilentlyContinue
Get-Process TiWorker -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$watch = [Diagnostics.Stopwatch]::StartNew()
$quiet = 0
while ($watch.Elapsed.TotalSeconds -lt 180 -and $quiet -lt 10) {
    if ((Get-Cpu) -lt 15) { $quiet++ } else { $quiet = 0 }
}
$idleAfterSeconds = [int]$watch.Elapsed.TotalSeconds
$busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object CPU | Sort-Object CPU -Descending | Select-Object -First 6 |
    ForEach-Object { "$($_.ProcessName) $([int]$_.CPU)s" })

$state = [hashtable]::Synchronized(@{ Stop = $false; Samples = New-Object System.Collections.ArrayList })
# The sampler runs in its own runspace and gets the shared $state as its one argument ($args[0]); it appends a sample
# every 2 s until the main script sets $state.Stop.
$sampler = [PowerShell]::Create().AddScript({
    $state = $args[0]
    while (-not $state.Stop) {
        $c = Get-Counter '\Memory\Committed Bytes', '\Memory\Available MBytes', '\Processor(_Total)\% Processor Time' -ErrorAction SilentlyContinue
        $v = @($c.CounterSamples | ForEach-Object CookedValue)
        [void]$state.Samples.Add([pscustomobject]@{ t = (Get-Date).ToString('HH:mm:ss'); committedMB = [int]($v[0] / 1MB); availableMB = [int]$v[1]; cpu = [int]$v[2] })
        Start-Sleep -Seconds 2
    }
}).AddArgument($state)
$handle = $sampler.BeginInvoke()
& "$tools\compile-bench.ps1" -SourceDir 'D:\m5src\src\cascadia\TerminalApp\AgentMaster\tests' `
    -VcVars 'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat' -Parallel 4 -OutFile "$share\results\compile-guest-mp4-4.json"
$state.Stop = $true
[void]$sampler.EndInvoke($handle)
$samples = @($state.Samples)
[ordered]@{
    tiWorkerBefore   = $before
    idleAfterSeconds = $idleAfterSeconds
    busyBefore       = $busy
    peakCommittedMB  = ($samples | Measure-Object committedMB -Maximum).Maximum
    minAvailableMB   = ($samples | Measure-Object availableMB -Minimum).Minimum
    meanCpu          = [int](($samples | Measure-Object cpu -Average).Average)
    samples          = $samples
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$share\results\compile-guest-mp4-4-samples.json"
