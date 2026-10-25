<#
.SYNOPSIS
    Measurement job: the compile benchmark in the guest, twice, with the Build Tools that 5.ps1 installed.

.DESCRIPTION
    Unzips the harness sources (a git archive of src\cascadia\TerminalApp\AgentMaster at 387b4e842, the same copy the
    host compiled) onto D: (the raw data disk), then runs compile-bench.ps1 twice at /MP4 with
    D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat: results\compile-guest-mp4-1.json (cold: nothing in the guest's
    file cache yet) and results\compile-guest-mp4-2.json (warm).

.OUTPUTS
    None; the two JSON files on the share.
#>
$ErrorActionPreference = 'Stop'
$share = '\\host.lan\Data'
$tools = 'C:\bs\tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Copy-Item "$share\tools\compile-bench.ps1" $tools -Force
if (Test-Path D:\m5src) { Remove-Item D:\m5src -Recurse -Force }
Expand-Archive -Path "$share\tools\m5src.zip" -DestinationPath D:\m5src -Force
$tests = 'D:\m5src\src\cascadia\TerminalApp\AgentMaster\tests'
$vcvars = 'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
foreach ($n in 1, 2) {
    & "$tools\compile-bench.ps1" -SourceDir $tests -VcVars $vcvars -Parallel 4 -OutFile "$share\results\compile-guest-mp4-$n.json"
}
