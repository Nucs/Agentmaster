<#
.SYNOPSIS
    Builds the checked-out commit with that commit's own tools\Build-Agentmaster.ps1 into a layout on D:; the build step
    of every build (DESIGN.md §11: the build steps come from the commit).

.DESCRIPTION
    - -Purge force first deletes the worktree's build outputs (bin\, obj\, every "Generated Files" folder, the
      package project's bin\, obj\ and AppPackages\) and the layout: a clean build. -Purge none keeps them: msbuild
      rebuilds what changed since the worktree's last build.
    - The build runs in its own pwsh process, its standard output into -Log (the guest agent streams the file's new
      lines to the coordinator as the build runs) and its standard error into -Log with ".err" added. Its TEMP is
      D:\tmp and vcpkg's binary cache D:\cache\vcpkg, both on the data disk, which outlives the VM's boots.
    - Afterwards the layout must hold AppxManifest.xml and Agentmaster.exe; one that does not fails the step even when
      msbuild said it succeeded.

.PARAMETER Worktree
    The worktree's folder (D:\src\<configuration>-<identity>).

.PARAMETER Configuration
    Release or Debug.

.PARAMETER Identity
    dev (Package-Dev.appxmanifest: AgentmasterDev) or release (-ReleaseIdentity: Agentmaster, what release.yml ships).

.PARAMETER Purge
    none or force.

.PARAMETER MaxCpu
    msbuild's /m (projects at once); the coordinator picks it from the VM's memory.

.PARAMETER ClMpCount
    The C++ compilers per project (CL_MPCount); likewise.

.PARAMETER Layout
    The package layout's folder (-LayoutDir), kept per worktree so the package step copies only what changed.

.PARAMETER Log
    The build's output file (the step's progress file).

.OUTPUTS
    The build's exit code and time, then "BUILD {json}".

.NOTES
    PowerShell 7. Exit codes: the build's own (0 = built); 4 when msbuild succeeded but the layout is incomplete.
#>
param(
    [Parameter(Mandatory)][string]$Worktree,
    [Parameter(Mandatory)][ValidateSet('Release', 'Debug')][string]$Configuration,
    [Parameter(Mandatory)][ValidateSet('dev', 'release')][string]$Identity,
    [Parameter(Mandatory)][ValidateSet('none', 'force')][string]$Purge,
    [Parameter(Mandatory)][int]$MaxCpu,
    [Parameter(Mandatory)][int]$ClMpCount,
    [Parameter(Mandatory)][string]$Layout,
    [Parameter(Mandatory)][string]$Log
)
$ErrorActionPreference = 'Stop'

if ($Purge -eq 'force') {
    $doomed = @("$Worktree\bin", "$Worktree\obj", "$Worktree\src\cascadia\CascadiaPackage\bin",
        "$Worktree\src\cascadia\CascadiaPackage\obj", "$Worktree\src\cascadia\CascadiaPackage\AppPackages", $Layout)
    $doomed += @(Get-ChildItem -Path "$Worktree\src" -Directory -Recurse -Filter 'Generated Files' -ErrorAction SilentlyContinue |
        ForEach-Object FullName)
    foreach ($dir in $doomed) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    }
    Write-Output "purged $($doomed.Count) folders"
}
New-Item -ItemType Directory -Force -Path (Split-Path $Log), $Layout | Out-Null
Remove-Item -LiteralPath $Log, "$Log.err" -ErrorAction SilentlyContinue

$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$Worktree\tools\Build-Agentmaster.ps1",
    '-Configuration', $Configuration, '-MaxCpu', $MaxCpu, '-ClMpCount', $ClMpCount, '-LayoutDir', $Layout)
if ($Identity -eq 'release') { $arguments += '-ReleaseIdentity' }
$env:TEMP = 'D:\tmp'
$env:TMP = 'D:\tmp'
$env:VCPKG_DEFAULT_BINARY_CACHE = 'D:\cache\vcpkg'
# msbuild's worker nodes otherwise outlive the build by up to 15 idle minutes (node reuse), holding memory the VM needs
# and the build's output handles open.
$env:MSBUILDDISABLENODEREUSE = '1'
$watch = [Diagnostics.Stopwatch]::StartNew()
# No -Wait: Start-Process -Wait waits for every DESCENDANT too, and the compiler server and mspdbsrv.exe stay up for
# minutes after a build (measured: the first build's step ran on long after "Build OK"). Only the build's own process
# decides when the step ends.
$build = Start-Process -FilePath "$env:ProgramFiles\PowerShell\7\pwsh.exe" -ArgumentList $arguments `
    -WorkingDirectory $Worktree -RedirectStandardOutput $Log -RedirectStandardError "$Log.err" -NoNewWindow -PassThru
# Taking the handle now keeps the process's exit code readable after it ends (a Start-Process object whose handle was
# never opened reports no ExitCode).
$null = $build.Handle
$build.WaitForExit()
$seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
$code = $build.ExitCode
Write-Output "build exit $code after $([math]::Round($seconds / 60, 1)) min ($Configuration, $Identity, purge $Purge, /m:$MaxCpu, CL_MPCount $ClMpCount)"
if ($code -eq 0 -and -not ((Test-Path "$Layout\AppxManifest.xml") -and (Test-Path "$Layout\Agentmaster.exe"))) {
    Write-Output "msbuild succeeded but $Layout has no AppxManifest.xml or Agentmaster.exe"
    $code = 4
}
if ($code -ne 0) {
    # The end of the build's own output is what explains a failure; the step's tail carries it to the request.
    Get-Content -LiteralPath $Log -Tail 30 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'error|failed|FAILED' } |
        Select-Object -Last 12 | ForEach-Object { Write-Output $_ }
    Get-Content -LiteralPath "$Log.err" -Tail 10 -ErrorAction SilentlyContinue | ForEach-Object { Write-Output $_ }
}
$facts = [ordered]@{
    exitCode      = $code
    seconds       = $seconds
    configuration = $Configuration
    identity      = $Identity
    purge         = $Purge
    maxCpu        = $MaxCpu
    clMpCount     = $ClMpCount
    freeD_GB      = [math]::Round((Get-PSDrive D).Free / 1GB, 1)
}
Write-Output ('BUILD ' + ($facts | ConvertTo-Json -Compress))
exit $code
