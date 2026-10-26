<#
.SYNOPSIS
    Makes the build VM's data disk D: on its first boot, and the folders a build uses; a step of every build
    (buildserver/src/Amb.Coordinator/Jobs/BuildJob.cs).

.DESCRIPTION
    The kept build VM has a second disk, a sparse raw file the slot driver attaches (data-disk.raw). At the VM's first
    boot it is RAW: this step initializes it (GPT), makes one partition over all of it and formats it NTFS as D:. At
    every later boot Windows mounts it as D: again, and this step only checks it.

    Then it makes D:\src (the worktrees, one per configuration and identity), D:\out (each worktree's layout and logs),
    D:\cache\vcpkg (vcpkg's binary cache, shared by the worktrees) and D:\tmp (the build's TEMP, so C:, the image's
    overlay, does not grow with temporary files).

.OUTPUTS
    One line per fact (disk, free space), then "DISK {json}".

.NOTES
    PowerShell 7 (the am-build image's). Exit codes: 0 D: is ready; 1 there is no D: and no RAW disk to make it from.
#>
$ErrorActionPreference = 'Stop'

$volume = Get-Volume -DriveLetter D -ErrorAction SilentlyContinue
$made = $false
if (-not $volume) {
    $raw = Get-Disk | Where-Object { $_.PartitionStyle -eq 'RAW' } | Sort-Object Number | Select-Object -First 1
    if (-not $raw) {
        Write-Output 'no D: and no RAW disk: the slot has no data disk attached'
        exit 1
    }
    # The first boot of this VM: the whole disk becomes D:.
    $raw | Initialize-Disk -PartitionStyle GPT -PassThru |
        New-Partition -DriveLetter D -UseMaximumSize |
        Format-Volume -FileSystem NTFS -NewFileSystemLabel 'build' -Confirm:$false | Out-Null
    $volume = Get-Volume -DriveLetter D
    $made = $true
}
foreach ($dir in 'D:\src', 'D:\out', 'D:\cache\vcpkg', 'D:\tmp') {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
$facts = [ordered]@{
    made     = $made
    label    = $volume.FileSystemLabel
    sizeGB   = [math]::Round($volume.Size / 1GB, 1)
    freeGB   = [math]::Round($volume.SizeRemaining / 1GB, 1)
    freeC_GB = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
}
$facts.GetEnumerator() | ForEach-Object { Write-Output ("{0,-9} {1}" -f $_.Key, $_.Value) }
Write-Output ('DISK ' + ($facts | ConvertTo-Json -Compress))
exit 0
