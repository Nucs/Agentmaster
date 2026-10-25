<#
.SYNOPSIS
    Measurement job: the same disk test on four disks that differ only in how QEMU attaches them, to find the
    attachment that gives a build VM the most random I/O.

.DESCRIPTION
    The slot boots with test disks whose serial numbers name their QEMU settings (bs-d3-wb-threads, bs-d4-blk,
    bs-d5-mq); D: (the first data disk, cache=none, aio=native, virtio-scsi) is the baseline. Each blank disk is
    initialized as GPT with one NTFS volume. On each volume: a 1 GB file is written sequentially (every block becomes
    data on the host), then DiskSpd measures random 4K overwrites at queue depth 32 and 1, and random 4K reads, with
    the OS cache off. results\attach.json, keyed by serial number (D: = 'baseline').

.OUTPUTS
    None; results\attach.json on the share.
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

function Measure-Volume {
    <#
    .SYNOPSIS
        Fills a 1 GB file on a volume, then measures random 4K overwrites (QD32, QD1) and reads over it.
    .PARAMETER Letter
        The volume's drive letter.
    .OUTPUTS
        [ordered] hashtable of the four numbers.
    #>
    param([string]$Letter)
    $file = "${Letter}:\a.dat"
    $buffer = New-Object byte[] (1MB)
    (New-Object Random 2).NextBytes($buffer)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $stream = New-Object IO.FileStream $file, 'Create', 'Write', 'None', (1MB), 'WriteThrough'
    for ($i = 0; $i -lt 1024; $i++) { $stream.Write($buffer, 0, $buffer.Length) }
    $stream.Flush($true)
    $stream.Dispose()
    $fill = $watch.Elapsed.TotalSeconds
    $r = [ordered]@{
        fillMBps          = [math]::Round(1024 / $fill, 1)
        overwriteRand4k   = Invoke-DiskSpd @('-d12', '-W2', '-Su', '-b4K', '-o32', '-t1', '-r', '-w100', $file)
        overwriteRand4kQ1 = Invoke-DiskSpd @('-d8', '-W2', '-Su', '-b4K', '-o1', '-t1', '-r', '-w100', $file)
        readRand4k        = Invoke-DiskSpd @('-d12', '-W2', '-Su', '-b4K', '-o32', '-t1', '-r', '-w0', $file)
        readRand4kT4      = Invoke-DiskSpd @('-d12', '-W2', '-Su', '-b4K', '-o8', '-t4', '-r', '-w0', $file)
    }
    Remove-Item $file -Force -ErrorAction SilentlyContinue
    $r
}

$letters = 'E', 'F', 'G', 'H'
$result = [ordered]@{ disks = @(Get-Disk | ForEach-Object { "$($_.Number) $($_.SerialNumber) $($_.BusType) $($_.PartitionStyle) $([math]::Round($_.Size/1GB))G" }) }
$i = 0
foreach ($disk in (Get-Disk | Where-Object { $_.PartitionStyle -eq 'RAW' } | Sort-Object Number)) {
    $letter = $letters[$i]; $i++
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT
    New-Partition -DiskNumber $disk.Number -UseMaximumSize -DriveLetter $letter | Out-Null
    Format-Volume -DriveLetter $letter -FileSystem NTFS -Confirm:$false | Out-Null
    $name = if ($disk.SerialNumber) { $disk.SerialNumber.Trim() } else { "disk$($disk.Number)" }
    $result[$name] = Measure-Volume $letter
}
$result['baseline'] = Measure-Volume 'D'
$result | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 '\\host.lan\Data\results\attach.json'
