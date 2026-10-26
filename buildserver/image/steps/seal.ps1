<#
.SYNOPSIS
    Waits until Windows' servicing is idle before the am-build layer is kept as an image (DESIGN.md §15).

.DESCRIPTION
    After the Build Tools install, Tiny11 Core's servicing worker (TiWorker.exe, under TrustedInstaller) kept burning
    about one vCPU for 30+ minutes (measured: 1,791 CPU-seconds) until TrustedInstaller was stopped. An image sealed
    in the middle of that would carry the work into every build VM it boots. So this step waits, up to -MaxMinutes,
    for TiWorker to be gone or idle (under 2 % of one CPU over a 30-s sample); then, if it is still busy, it stops
    TrustedInstaller. Either way it reports what it saw, and `images.py check --image am-build` checks at the next
    boot that servicing stays idle.

.PARAMETER MaxMinutes
    How long to wait for servicing to go idle by itself before stopping TrustedInstaller (default 20).

.OUTPUTS
    One line per sample, then "SEAL {json}".

.NOTES
    PowerShell 5.1. Exit code 0 always: a busy servicing worker is stopped, not a reason to lose a 30-min install.
#>
param([int]$MaxMinutes = 20)
$ErrorActionPreference = 'Continue'

function Get-ServicingCpuShare {
    <#
    .SYNOPSIS
        The share of one CPU that TiWorker used over a sample.
    .PARAMETER Seconds
        The sample's length.
    .OUTPUTS
        [double] 0..N (1.0 = one full CPU); 0 when TiWorker is not running.
    #>
    param([int]$Seconds = 30)
    $cpu0 = (Get-Process -Name TiWorker -ErrorAction SilentlyContinue |
        ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum
    Start-Sleep -Seconds $Seconds  # a sampling interval, not a wait for a condition
    $cpu1 = (Get-Process -Name TiWorker -ErrorAction SilentlyContinue |
        ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum
    if ($null -eq $cpu1) { return 0.0 }
    if ($null -eq $cpu0) { $cpu0 = 0.0 }
    # A worker that restarted between the samples counts from zero, never as a negative share.
    return [math]::Round([math]::Max(0.0, ([double]$cpu1 - [double]$cpu0) / $Seconds), 3)
}

$watch = [Diagnostics.Stopwatch]::StartNew()
$share = 1.0
while ($watch.Elapsed.TotalMinutes -lt $MaxMinutes) {
    $share = Get-ServicingCpuShare -Seconds 30
    Write-Output ("{0,5:0.0} min  TiWorker {1:P0} of a CPU" -f $watch.Elapsed.TotalMinutes, $share)
    if ($share -lt 0.02) { break }
}
$stopped = $false
if ($share -ge 0.02) {
    Stop-Service -Name TrustedInstaller -Force -ErrorAction SilentlyContinue
    $stopped = $true
}
$summary = [ordered]@{
    waitedMinutes      = [math]::Round($watch.Elapsed.TotalMinutes, 1)
    lastCpuShare       = $share
    trustedInstaller   = (Get-Service -Name TrustedInstaller -ErrorAction SilentlyContinue).Status.ToString()
    stoppedByThisStep  = $stopped
}
Write-Output ('SEAL ' + ($summary | ConvertTo-Json -Compress))
exit 0
