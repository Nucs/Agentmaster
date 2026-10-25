<#
.SYNOPSIS
    Measurement job: are the slot's slow random writes the cost of writing NEW space (block allocation in the host's
    qcow2 or ext4), or of every write?

.DESCRIPTION
    On C: (the qcow2 overlay) and on D: (the sparse raw file): writes a 1 GB file sequentially so every block of it is
    allocated on the host (timed: the speed of writing new data), then measures random 4K writes over that same file
    (overwrites, the device cache allowed) and random 4K reads, 15 s each, with DiskSpd. results\alloc.json.

.OUTPUTS
    None; results\alloc.json on the share.
#>
$ErrorActionPreference = 'Continue'
$diskspd = 'C:\bs\tools\diskspd.exe'

function Invoke-DiskSpd {
    <#
    .SYNOPSIS
        Runs DiskSpd and returns the summary's read+write total.
    .PARAMETER Arguments
        DiskSpd's switches and the target file, one element each.
    .OUTPUTS
        [ordered] @{ MiBps; iops }, or the failure's text.
    #>
    param([string[]]$Arguments)
    $text = & $diskspd $Arguments 2>&1 | Out-String
    $line = ($text -split "`r?`n" | Where-Object { $_ -match '^\s*total:' } | Select-Object -First 1)
    if (-not $line) { return "failed: $($text.Substring(0, [Math]::Min(200, $text.Length)))" }
    $cells = ($line -replace '^\s*total:\s*', '') -split '\|' | ForEach-Object { $_.Trim() }
    [ordered]@{ MiBps = [double]$cells[2]; iops = [double]$cells[3] }
}

$result = [ordered]@{}
foreach ($drive in 'C', 'D') {
    $dir = "${drive}:\alloc"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $file = "$dir\a.dat"
    Remove-Item $file -Force -ErrorAction SilentlyContinue
    # A plain sequential write of 1 GB through .NET, then a flush to the device: every block becomes new data.
    $buffer = New-Object byte[] (1MB)
    (New-Object Random 2).NextBytes($buffer)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $stream = New-Object IO.FileStream $file, 'Create', 'Write', 'None', (1MB), 'WriteThrough'
    for ($i = 0; $i -lt 1024; $i++) { $stream.Write($buffer, 0, $buffer.Length) }
    $stream.Flush($true)
    $stream.Dispose()
    $fillSeconds = $watch.Elapsed.TotalSeconds
    $result[$drive] = [ordered]@{
        fillMBps          = [math]::Round(1024 / $fillSeconds, 1)
        overwriteRand4k   = Invoke-DiskSpd @('-d15', '-W2', '-Su', '-b4K', '-o32', '-t1', '-r', '-w100', $file)
        overwriteRand4kQ1 = Invoke-DiskSpd @('-d10', '-W2', '-Su', '-b4K', '-o1', '-t1', '-r', '-w100', $file)
        readRand4k        = Invoke-DiskSpd @('-d15', '-W2', '-Su', '-b4K', '-o32', '-t1', '-r', '-w0', $file)
    }
    Remove-Item $file -Force -ErrorAction SilentlyContinue
}
$result | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 '\\host.lan\Data\results\alloc.json'
