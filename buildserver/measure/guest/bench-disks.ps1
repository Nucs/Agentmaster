<#
.SYNOPSIS
    Measurement job: brings the slot's raw data disk online as D: and runs bench.ps1 on C: (the qcow2 overlay over the
    golden image) and on D: (the sparse raw file), both stored in the dedicated distro's ext4 on the host's Q:.

.DESCRIPTION
    The data disk arrives blank (a sparse file the host made with truncate): it is initialized as GPT, given one NTFS
    partition as D: with a quick format, and timed. Then bench.ps1 runs twice with 4 threads; the results go to
    results\bench-q-c.json and results\bench-q-d.json, and the disk preparation's timing to results\disk-d.json.

.OUTPUTS
    None; the JSON files on the share.
#>
$ErrorActionPreference = 'Stop'
$tools = 'C:\bs\tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Copy-Item '\\host.lan\Data\tools\bench.ps1', '\\host.lan\Data\tools\diskspd.exe' $tools -Force
$watch = [Diagnostics.Stopwatch]::StartNew()
$disk = Get-Disk | Where-Object PartitionStyle -eq 'RAW' | Select-Object -First 1
Initialize-Disk -Number $disk.Number -PartitionStyle GPT
$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -DriveLetter D
Format-Volume -DriveLetter D -FileSystem NTFS -NewFileSystemLabel data -Confirm:$false | Out-Null
[ordered]@{ diskNumber = $disk.Number; sizeGB = [math]::Round($disk.Size / 1GB, 1); prepareSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 1) } |
    ConvertTo-Json | Set-Content -Encoding utf8 '\\host.lan\Data\results\disk-d.json'
& "$tools\bench.ps1" -OutFile '\\host.lan\Data\results\bench-q-c.json' -WorkDir 'C:\bs\bench' -Threads 4
& "$tools\bench.ps1" -OutFile '\\host.lan\Data\results\bench-q-d.json' -WorkDir 'D:\bench' -Threads 4
