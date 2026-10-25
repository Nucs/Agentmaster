<#
.SYNOPSIS
    The measurement job runner of a build-server test VM: a stand-in for the design's guest agent.

.DESCRIPTION
    Runs at the auto-logon user's logon (scheduled task BsAgent, registered by setup.ps1), elevated. It polls dockur's
    shared folder (\\host.lan\Data, the host's /shared folder) and runs each job one at a time:
      jobs\<n>.ps1     the numbered queue: n = 1, 2, 3, ...; the agent asks for exactly the next number by its path;
      jobs\<name>.ps1  any other name, found by listing the folder (the first version's way, kept as a fallback);
    A job is copied to C:\bs\jobs and run with PowerShell 5.1; results\<name>.log gets its output, results\<name>.exit
    "<exit code> <seconds>"; the job file is renamed <name>.ps1.done before it runs, so it never runs twice.
    At start it writes results\agent-start-<yyyyMMdd-HHmmss>.json: the boot phases, memory and disk, which is how the
    host measures boot-to-ready time. Nothing listens in the guest: the host only drops files.

.OUTPUTS
    None; it runs until the VM shuts down.

.NOTES
    Why numbered jobs, asked for by path: the guest's SMB client holds a lease on the jobs folder and serves its
    listing from cache until Samba breaks the lease, and Samba never learns of a file the Windows host writes through
    Docker Desktop's bind mount (no change notification crosses it). Seen 2026-10-25: a job written from the host was
    never listed. A path the client has not seen yet goes to the server once its not-found cache expires, which this
    script also turns off. The last number done is kept in C:\bs\last-job.txt, so a reboot does not run a job again.
    A job that throws or exits non-zero is recorded and the loop goes on.
#>
$ErrorActionPreference = 'Continue'
$share = '\\host.lan\Data'
$local = 'C:\bs\jobs'
$lastFile = 'C:\bs\last-job.txt'
New-Item -ItemType Directory -Force -Path $local | Out-Null

function Get-MachineFacts {
    <#
    .SYNOPSIS
        Collects the facts a report needs: boot phases, uptime, memory, commit, the system disk, process count.
    .PARAMETER ShareReady
        When the shared folder first answered; with the boot, shell and agent start times it splits boot-to-ready
        into its phases (kernel to shell = boot and logon, shell to agent = the logon task, agent to share = network).
    .OUTPUTS
        [ordered] hashtable, ready for ConvertTo-Json.
    #>
    param([datetime]$ShareReady)
    $os = Get-CimInstance Win32_OperatingSystem
    $disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'"
    $committed = (Get-Counter '\Memory\Committed Bytes' -ErrorAction SilentlyContinue).CounterSamples.CookedValue
    $boot = $os.LastBootUpTime
    $shell = (Get-Process explorer -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -First 1).StartTime
    $agent = (Get-Process -Id $PID).StartTime
    [ordered]@{
        now            = (Get-Date).ToString('o')
        lastBoot       = $boot.ToString('o')
        uptimeSeconds  = [int]((Get-Date) - $boot).TotalSeconds
        shellAfterBootSeconds = if ($shell) { [math]::Round(($shell - $boot).TotalSeconds, 1) } else { $null }
        agentAfterBootSeconds = [math]::Round(($agent - $boot).TotalSeconds, 1)
        shareAfterBootSeconds = if ($ShareReady) { [math]::Round(($ShareReady - $boot).TotalSeconds, 1) } else { $null }
        caption        = $os.Caption
        build          = $os.BuildNumber
        ramTotalMB     = [int]($os.TotalVisibleMemorySize / 1KB)
        ramFreeMB      = [int]($os.FreePhysicalMemory / 1KB)
        commitMB       = [int]($committed / 1MB)
        commitLimitMB  = [int]($os.TotalVirtualMemorySize / 1KB)
        diskSizeGB     = [math]::Round($disk.Size / 1GB, 2)
        diskFreeGB     = [math]::Round($disk.FreeSpace / 1GB, 2)
        processes      = @(Get-Process).Count
        servicesRunning = @(Get-Service | Where-Object Status -eq 'Running').Count
    }
}

function Wait-Share {
    <#
    .SYNOPSIS
        Waits until the shared folder answers, then makes sure its jobs and results folders exist.
    .OUTPUTS
        None. Loops forever while the share is unreachable (the VM is useless without it).
    #>
    while (-not (Test-Path "$share\")) { Start-Sleep -Seconds 2 }
    foreach ($sub in 'jobs', 'results') { New-Item -ItemType Directory -Force -Path "$share\$sub" | Out-Null }
}

function Invoke-Job {
    <#
    .SYNOPSIS
        Runs one job file from the share and records its output and exit code.
    .PARAMETER Path
        The job's path on the share (jobs\<name>.ps1).
    .OUTPUTS
        None. results\<name>.log and results\<name>.exit on the share.
    #>
    param([string]$Path)
    $leaf = Split-Path $Path -Leaf
    $name = [IO.Path]::GetFileNameWithoutExtension($leaf)
    $copy = Join-Path $local $leaf
    Copy-Item $Path $copy -Force
    # Renamed before it runs: a job that reboots or kills the agent must not run again at the next logon.
    Rename-Item $Path "$leaf.done" -Force
    $started = Get-Date
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $copy *> "$share\results\$name.log"
    $code = $LASTEXITCODE
    "$code $([int]((Get-Date) - $started).TotalSeconds)" | Set-Content -Encoding ascii "$share\results\$name.exit"
}

# No SMB client caching of folder listings, file facts or "not found" answers (see .NOTES); best effort.
try {
    Set-SmbClientConfiguration -DirectoryCacheLifetime 0 -FileInfoCacheLifetime 0 -FileNotFoundCacheLifetime 0 `
        -Confirm:$false -ErrorAction Stop
} catch { }

Wait-Share
$shareReadyAt = Get-Date
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Get-MachineFacts -ShareReady $shareReadyAt | ConvertTo-Json | Set-Content -Encoding utf8 "$share\results\agent-start-$stamp.json"

$last = if (Test-Path $lastFile) { [int](Get-Content $lastFile -Raw) } else { 0 }
while ($true) {
    if (-not (Test-Path "$share\jobs")) { Wait-Share }
    $next = Join-Path "$share\jobs" "$($last + 1).ps1"
    if (Test-Path -LiteralPath $next) {
        $last++
        Set-Content -Encoding ascii $lastFile $last
        Invoke-Job $next
        continue
    }
    $named = Get-ChildItem "$share\jobs" -Filter '*.ps1' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -notmatch '^\d+$' } | Sort-Object Name
    foreach ($job in $named) { Invoke-Job $job.FullName }
    Start-Sleep -Seconds 2
}
