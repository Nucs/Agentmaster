<#
.SYNOPSIS
    Measurement job: does Agentmaster's portable release (v0.8.0) start on Tiny11 Core, and what does it cost at rest?

.DESCRIPTION
    Unzips \\host.lan\Data\tools\Agentmaster_0.8.0.0_x64.zip into C:\am, makes it a kept-portable copy with its
    profile beside it (install.path = portable, profile.path = profile, so neither first-launch question appears),
    starts Agentmaster.exe, waits 30 s, and writes results\app-core11.json: whether it still runs, its window title,
    its memory, the console hosts and shells it started, and the guest's memory before and after.
    The app is left running, so the host can take a screenshot of it.

.OUTPUTS
    None; results\app-core11.json on the share.
#>
$ErrorActionPreference = 'Continue'
$share = '\\host.lan\Data'

function Get-AvailableMB {
    <#
    .SYNOPSIS
        The guest's available physical memory in MB.
    .OUTPUTS
        [int]
    #>
    [int](Get-Counter '\Memory\Available MBytes').CounterSamples.CookedValue
}

$before = Get-AvailableMB
$zip = "$share\tools\Agentmaster_0.8.0.0_x64.zip"
$watch = [Diagnostics.Stopwatch]::StartNew()
if (Test-Path C:\am) { Remove-Item C:\am -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath C:\am -Force
$unzipSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
$exe = Get-ChildItem C:\am -Recurse -Filter Agentmaster.exe | Select-Object -First 1
$dir = $exe.DirectoryName
Set-Content -Encoding ascii "$dir\install.path" 'portable'
Set-Content -Encoding ascii "$dir\profile.path" 'profile'
$app = Start-Process -FilePath $exe.FullName -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds 30
$app.Refresh()
$children = Get-CimInstance Win32_Process | Where-Object { $_.Name -in 'OpenConsole.exe', 'powershell.exe', 'pwsh.exe', 'cmd.exe' }
$after = Get-AvailableMB
[ordered]@{
    unzipSeconds      = $unzipSeconds
    exe               = $exe.FullName
    running           = -not $app.HasExited
    exitCode          = if ($app.HasExited) { $app.ExitCode } else { $null }
    windowTitle       = $app.MainWindowTitle
    workingSetMB      = [int]($app.WorkingSet64 / 1MB)
    privateMB         = [int]($app.PrivateMemorySize64 / 1MB)
    threads           = $app.Threads.Count
    consoleChildren   = @($children | ForEach-Object { "$($_.Name) pid $($_.ProcessId) parent $($_.ParentProcessId)" })
    availableBeforeMB = $before
    availableAfterMB  = $after
    profileFiles      = @(Get-ChildItem "$dir\profile" -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object Name)
    hooksLogTail      = if (Test-Path "$dir\profile\hooks.log") { @(Get-Content "$dir\profile\hooks.log" -Tail 15) } else { @() }
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$share\results\app-core11.json"
