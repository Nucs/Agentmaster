<#
.SYNOPSIS
    Measurement job: the same small benchmark set for a build-server test VM and for the host, so their speeds compare.

.DESCRIPTION
    Measures what a build or an e2e run spends its time on, with nothing but PowerShell 5.1 and .NET Framework
    (plus DiskSpd when it is next to this script):
    - cpu1 / cpuN    managed SHA-256 over a 64 MB buffer, on one thread and on -Threads runspaces at once (MB/s);
    - process        100 starts of 'cmd.exe /c exit', one after another (ms per process; compilers and MSBuild
                     start thousands of processes);
    - smallFiles     5,000 files of 4 KB created, read back and deleted in -WorkDir (files/s per phase; a build's
                     obj trees are small files);
    - disk           DiskSpd on a 2 GB file in -WorkDir with the OS cache off: random 4K at queue depth 32 (write
                     with every write forced through, write with the device cache allowed, read) and sequential 1M
                     at depth 4 (write, read), 15 s each (IOPS, MiB/s).
    Each result carries its wall time, so a slow phase is visible on its own.

.PARAMETER OutFile
    Where the JSON result goes.

.PARAMETER WorkDir
    A folder on the disk under test; created when missing; its test files are deleted at the end.

.PARAMETER Threads
    The runspaces for the parallel CPU test; defaults to the logical processor count.

.PARAMETER SkipDisk
    Leaves out the DiskSpd tests (they need diskspd.exe beside this script and ~2 GB free).

.OUTPUTS
    None; the JSON goes to -OutFile.

.NOTES
    A test that fails records its error message in place of its numbers; the others still run.
#>
param(
    [Parameter(Mandatory)] [string]$OutFile,
    [Parameter(Mandatory)] [string]$WorkDir,
    [int]$Threads = [Environment]::ProcessorCount,
    [switch]$SkipDisk
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$result = [ordered]@{ host = $env:COMPUTERNAME; logicalCpus = [Environment]::ProcessorCount; threads = $Threads; workDir = $WorkDir }

# The hashing body shared by the single- and multi-thread tests: managed SHA-256 (no CPU extensions), so the number
# depends on the core's integer speed only, the same way on the host and in a guest.
$hashScript = {
    param([int]$MegaBytes, [int]$Repeats)
    $buffer = New-Object byte[] ($MegaBytes * 1MB)
    (New-Object Random 1).NextBytes($buffer)
    $sha = New-Object System.Security.Cryptography.SHA256Managed
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt $Repeats; $i++) { [void]$sha.ComputeHash($buffer) }
    $watch.Elapsed.TotalSeconds
}

$hashRepeats = 16
try {
    $seconds = & $hashScript 64 $hashRepeats
    $result.cpu1 = [ordered]@{ MBps = [math]::Round(64 * $hashRepeats / $seconds, 1); seconds = [math]::Round($seconds, 2) }
} catch { $result.cpu1 = "failed: $($_.Exception.Message)" }

try {
    # One runspace per thread, all opened before any work starts (Open with the minimum = the maximum makes every
    # runspace up front), so the hashing loops overlap. Each loop times itself: the slowest loop is the
    # parallel time, without the runspaces' start-up and the buffers' random fill.
    $pool = [RunspaceFactory]::CreateRunspacePool($Threads, $Threads)
    $pool.Open()
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $running = foreach ($i in 1..$Threads) {
        $ps = [PowerShell]::Create().AddScript($hashScript).AddArgument(64).AddArgument($hashRepeats)
        $ps.RunspacePool = $pool
        [pscustomobject]@{ Shell = $ps; Handle = $ps.BeginInvoke() }
    }
    $loopSeconds = foreach ($r in $running) { [double]($r.Shell.EndInvoke($r.Handle) | Select-Object -Last 1); $r.Shell.Dispose() }
    $wall = $watch.Elapsed.TotalSeconds
    $pool.Close()
    $slowest = ($loopSeconds | Measure-Object -Maximum).Maximum
    $result.cpuN = [ordered]@{
        MBps = [math]::Round($Threads * 64 * $hashRepeats / $slowest, 1)
        slowestLoopSeconds = [math]::Round($slowest, 2); wallSeconds = [math]::Round($wall, 2)
    }
} catch { $result.cpuN = "failed: $($_.Exception.Message)" }

try {
    $info = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', '/c exit'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt 100; $i++) { $p = [Diagnostics.Process]::Start($info); $p.WaitForExit(); $p.Dispose() }
    $result.process = [ordered]@{ msPerProcess = [math]::Round($watch.Elapsed.TotalMilliseconds / 100, 2) }
} catch { $result.process = "failed: $($_.Exception.Message)" }

try {
    $dir = Join-Path $WorkDir 'small'
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item -ItemType Directory -Path $dir | Out-Null
    $bytes = New-Object byte[] 4096
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt 5000; $i++) { [IO.File]::WriteAllBytes("$dir\f$i.bin", $bytes) }
    $create = $watch.Elapsed.TotalSeconds
    $watch.Restart()
    for ($i = 0; $i -lt 5000; $i++) { [void][IO.File]::ReadAllBytes("$dir\f$i.bin") }
    $read = $watch.Elapsed.TotalSeconds
    $watch.Restart()
    Remove-Item $dir -Recurse -Force
    $delete = $watch.Elapsed.TotalSeconds
    $result.smallFiles = [ordered]@{
        createPerSec = [int](5000 / $create); readPerSec = [int](5000 / $read); deletePerSec = [int](5000 / $delete)
    }
} catch { $result.smallFiles = "failed: $($_.Exception.Message)" }

$diskspd = Join-Path $PSScriptRoot 'diskspd.exe'
if (-not $SkipDisk -and (Test-Path $diskspd)) {
    $file = Join-Path $WorkDir 'diskspd.dat'
    # -Sh = no OS cache and no device write cache: every write is forced through (FUA), the worst case.
    # -Su = no OS cache, device write cache allowed: what a program's writes look like once the lazy writer hands
    # them to the disk, so it is the number a build's writes meet. Sequential is DiskSpd's default without -r.
    $tests = [ordered]@{
        randWrite4kForced = '-Sh -b4K -o32 -t1 -r -w100'
        randWrite4k       = '-Su -b4K -o32 -t1 -r -w100'
        randRead4k        = '-Su -b4K -o32 -t1 -r -w0'
        seqWrite1m        = '-Su -b1M -o4 -t1 -w100'
        seqRead1m         = '-Su -b1M -o4 -t1 -w0'
    }
    $disk = [ordered]@{}
    foreach ($name in $tests.Keys) {
        try {
            # One array element per switch, the file last: PowerShell quotes an element only when it needs to.
            $argList = @('-c2G', '-d15', '-W2', '-L') + $tests[$name].Split(' ') + @($file)
            # DiskSpd writes warnings to stderr; under 'Stop' PowerShell 5.1 would turn the first one into an error.
            $ErrorActionPreference = 'Continue'
            $text = & $diskspd $argList 2>&1 | Out-String
            $ErrorActionPreference = 'Stop'
            # The first 'total:' line of the summary is the read+write total: bytes | I/Os | MiB/s | I/O per s | ...
            $line = ($text -split "`r?`n" | Where-Object { $_ -match '^\s*total:' } | Select-Object -First 1)
            $cells = ($line -replace '^\s*total:\s*', '') -split '\|' | ForEach-Object { $_.Trim() }
            $disk[$name] = [ordered]@{ MiBps = [double]$cells[2]; iops = [double]$cells[3] }
        } catch { $disk[$name] = "failed: $($_.Exception.Message)" }
    }
    Remove-Item $file -Force -ErrorAction SilentlyContinue
    $result.disk = $disk
}

$result | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $OutFile
