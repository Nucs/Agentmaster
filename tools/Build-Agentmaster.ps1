#Requires -Version 7
<#
.SYNOPSIS
  Fast build wrapper for Agentmaster (fork of Windows Terminal).

.DESCRIPTION
  Fixes the three reasons the stock `Invoke-OpenConsoleBuild` is slow:
    1. Adds /m  -> builds projects in PARALLEL (stock build is single-node).
       (Per-file /MP is already enabled in src/common.build.pre.props.)
    2. Scopes to ONE config/platform and just the Terminal app target
       (Terminal\CascadiaPackage) instead of the whole solution
       (tests, tools, samples, fuzzers).
    3. Skips the redundant double `nuget restore` on incremental rebuilds
       (use -NoRestore once the first restore has succeeded).

  Tuned defaults for this machine (i9-13900K = 32 threads, 64 GB RAM):
  unbounded /m + /MP fits comfortably. If you ever see paging, pass
  -ClMpCount 6 (or -MaxCpu 16) to cap concurrency.

.EXAMPLE
  # First build (restores, parallel, app only):
  .\tools\Build-Agentmaster.ps1

.EXAMPLE
  # Fast inner-loop rebuild after editing TerminalApp:
  .\tools\Build-Agentmaster.ps1 -NoRestore

.EXAMPLE
  # Whole solution incl. tests (e.g. before running unit tests):
  .\tools\Build-Agentmaster.ps1 -Full
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'AuditMode')][string]$Configuration = 'Debug',
    [ValidateSet('x64', 'Win32', 'ARM64')][string]$Platform = 'x64',
    # Solution-relative target. Default builds only the app + its dependency graph.
    [string]$Target = 'Terminal\CascadiaPackage',
    [int]$MaxCpu = 0, # 0 => /m (msbuild uses logical CPU count = 32 here)
    [int]$ClMpCount = 0, # 0 => unbounded /MP (already on); set e.g. 6 to cap RAM use
    [switch]$Full, # build the entire solution instead of just the app
    [switch]$Clean,
    [switch]$NoRestore
)

$ErrorActionPreference = 'Stop'

$root = (git -C $PSScriptRoot rev-parse --show-toplevel 2>$null)
if (-not $root) { $root = (Resolve-Path "$PSScriptRoot\..").Path }

# Ensure msbuild + the VC toolset are on PATH for this session.
if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    Import-Module "$root\tools\OpenConsole.psm1"
    Set-MsbuildDevEnvironment
}

if (-not $NoRestore) {
    & "$root\dep\nuget\nuget.exe" restore "$root\OpenConsole.slnx"
    & "$root\dep\nuget\nuget.exe" restore "$root\dep\nuget\packages.config"
}

$mFlag = if ($MaxCpu -gt 0) { "/m:$MaxCpu" } else { '/m' }

$msbuildArgs = @(
    "$root\OpenConsole.slnx",
    $mFlag,
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    '/nologo',
    '/v:m'
)
if ($ClMpCount -gt 0) { $msbuildArgs += "/p:CL_MPCount=$ClMpCount" }
if ($Clean) { $msbuildArgs += '/t:Clean' }
elseif (-not $Full -and $Target) { $msbuildArgs += "/t:$Target" }

Write-Host "msbuild $($msbuildArgs -join ' ')" -ForegroundColor DarkGray
$sw = [Diagnostics.Stopwatch]::StartNew()
msbuild.exe @msbuildArgs
$code = $LASTEXITCODE
$sw.Stop()

if ($code -eq 0) {
    Write-Host ("Build OK in {0:n1}s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
}
else {
    Write-Host ("Build FAILED ({0}) after {1:n1}s" -f $code, $sw.Elapsed.TotalSeconds) -ForegroundColor Red
}
exit $code
