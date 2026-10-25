<#
.SYNOPSIS
    Measurement job: runs bench.ps1 in the guest on its own system disk (the qcow2 in the Docker volume on the host's C:).

.DESCRIPTION
    Copies bench.ps1 and diskspd.exe from the share to C:\bs\tools (so the script runs from the local disk and finds
    diskspd beside it), then runs it with 4 threads, the guest's vCPU count. The result goes to
    results\bench-core11-c.json.

.OUTPUTS
    None; the JSON on the share.
#>
$ErrorActionPreference = 'Stop'
$tools = 'C:\bs\tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Copy-Item '\\host.lan\Data\tools\bench.ps1', '\\host.lan\Data\tools\diskspd.exe' $tools -Force
& "$tools\bench.ps1" -OutFile '\\host.lan\Data\results\bench-core11-c.json' -WorkDir 'C:\bs\bench' -Threads 4
