<#
.SYNOPSIS
    Compiles the commit's engine test harness (m5) in the build VM's worktree and zips it as the build's test artifact,
    m5-tests.zip, which `amb test <build id> m5` runs in a fresh VM (TestJob): the tests step of every build, between the
    build step and the package step (DESIGN.md §13, `amb test`).

.DESCRIPTION
    - The commit's own runner compiles the harness: src\cascadia\TerminalApp\AgentMaster\tests\run-m5-tests.bat with
      --compile-only (cl /MP over the harness's own and the engine's .cpp files, no msbuild), so the bytes `amb test`
      runs are exactly those the commit's runner makes. A commit whose runner does not know --compile-only (one made
      before 2026-11-13) gets no artifact: exit 3, and the build goes on.
    - It runs after the build step because the harness takes two inputs from the tree that build made: cmark from the
      vcpkg tree (obj\x64\vcpkg\x64-windows-static; without it the Markdown and conversation-image tests report a skip)
      and the WebView2 SDK from packages\ (without it the update prompt compiles its plain-text path). The facts line
      says whether each was there.
    - MSVC: the newest Visual Studio with the C++ tools, by vswhere (the am-build image's Build Tools in C:\BuildTools),
      handed to the runner as AM_VCVARS; without vswhere, C:\BuildTools' own vcvars64.bat; else the runner searches by
      itself.
    - cmd.exe writes the runner's output into -Out\m5-compile.log itself: a pipe into PowerShell would stay open as
      long as any process that inherited it lives (vctip.exe, which cl starts, can outlive the compile), and the step
      would wait for it.
    - The zip holds m5_tests.exe, the manifest the linker wrote beside it (m5_tests.exe.manifest: Common Controls 6,
      which the harness's dialog code needs) and the data folders the harness reads beside its exe (turns\, voice\).
      The exe and its manifest are deleted before the compile, so a failed compile can never zip an earlier one.

.PARAMETER Worktree
    The worktree's folder (D:\src\<configuration>-<identity>), built by the build step.

.PARAMETER Out
    The share's folder of the request's artifacts (Z:\out\<request id>): m5-tests.zip and m5-compile.log go there,
    and the package step lists them in build.json with the rest.

.PARAMETER Temp
    The folder TEMP and TMP point to while the harness compiles: the data disk's D:\tmp in the build VM (as the build
    and package steps use), which outlives the VM's boots. A trial run elsewhere names its own.

.OUTPUTS
    The compile's result and time, then "TESTS {json}".

.NOTES
    PowerShell 7. Exit codes: 0 = m5-tests.zip made; 3 = the commit's runner cannot compile without running (no
    artifact; an older commit); 2 = the compile or the zip failed (m5-compile.log says why). The build job treats every
    one of them as a result, never as a failed build: the app was built either way.
#>
param(
    [Parameter(Mandatory)][string]$Worktree,
    [Parameter(Mandatory)][string]$Out,
    [string]$Temp = 'D:\tmp'
)
$ErrorActionPreference = 'Stop'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$env:TEMP = $Temp
$env:TMP = $Temp
$watch = [Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Force -Path $Out, $env:TEMP | Out-Null

$tests = Join-Path $Worktree 'src\cascadia\TerminalApp\AgentMaster\tests'
$runner = Join-Path $tests 'run-m5-tests.bat'
$log = Join-Path $Out 'm5-compile.log'
$zip = Join-Path $Out 'm5-tests.zip'

# Writes the facts line and ends the step with an exit code (every outcome ends here, so every outcome has one line).
function Complete([int]$code, [System.Collections.Specialized.OrderedDictionary]$facts) {
    $facts['exitCode'] = $code
    $facts['seconds'] = [math]::Round($watch.Elapsed.TotalSeconds, 1)
    Write-Output ('TESTS ' + ($facts | ConvertTo-Json -Compress -Depth 4))
    exit $code
}

# An older commit's runner compiles AND runs in one go: running the harness here would test in the build VM, against
# its state, which `amb test` exists to avoid. Such a commit gets no artifact.
if (-not (Test-Path -LiteralPath $runner) -or -not (Select-String -LiteralPath $runner -SimpleMatch '--compile-only' -Quiet)) {
    Write-Output "no m5 artifact: this commit's run-m5-tests.bat cannot compile without running (no --compile-only)"
    Complete 3 ([ordered]@{ kind = 'm5'; made = $false; reason = 'no --compile-only runner' })
}

# MSVC for the runner: what the build itself found (VSSetup there; vswhere here: the same installer's answer).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vcvars = $null
if (Test-Path -LiteralPath $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs -and (Test-Path -LiteralPath "$vs\VC\Auxiliary\Build\vcvars64.bat")) { $vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat" }
}
if (-not $vcvars -and (Test-Path -LiteralPath 'C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat')) {
    $vcvars = 'C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
}
if ($vcvars) { $env:AM_VCVARS = $vcvars }

$cmark = Test-Path -LiteralPath "$Worktree\obj\x64\vcpkg\x64-windows-static\include\cmark.h"
$webView2 = @(Get-ChildItem -Path "$Worktree\packages" -Directory -Filter 'Microsoft.Web.WebView2.*' -ErrorAction SilentlyContinue).Count -gt 0
$facts = [ordered]@{ kind = 'm5'; made = $false; vcvars = $vcvars; cmark = $cmark; webView2 = $webView2 }

# Cleanup before the compile: an exe from an earlier build of this worktree must never be zipped as this one's.
foreach ($stale in 'm5_tests.exe', 'm5_tests.exe.manifest') {
    Remove-Item -LiteralPath (Join-Path $tests $stale) -Force -ErrorAction SilentlyContinue
}

# cmd.exe redirects into the log itself (see .DESCRIPTION: no pipe for a lingering vctip.exe to hold open). The runner
# changes to its own folder after vcvars, so the working folder only matters for finding the runner, and ".\" finds it
# even where NoDefaultCurrentDirectoryInExePath is set (cmd then never searches the current folder: a Claude session's
# environment sets it, and the trial run of 2026-11-13 failed "not recognized" without the ".\").
Push-Location -LiteralPath $tests
try {
    & cmd.exe /d /c ".\run-m5-tests.bat --compile-only > `"$log`" 2>&1"
    $compileCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
$facts['compileSeconds'] = [math]::Round($watch.Elapsed.TotalSeconds, 1)
$exe = Join-Path $tests 'm5_tests.exe'
if ($compileCode -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Write-Output "the m5 harness did not compile (exit $compileCode); the end of m5-compile.log:"
    Get-Content -LiteralPath $log -Tail 25 -ErrorAction SilentlyContinue | ForEach-Object { Write-Output "  $_" }
    $facts['compileExitCode'] = $compileCode
    Complete 2 $facts
}

# The zip: the exe, its manifest when the linker wrote one, and the data folders the harness reads beside its exe.
$items = @('m5_tests.exe')
if (Test-Path -LiteralPath (Join-Path $tests 'm5_tests.exe.manifest')) { $items += 'm5_tests.exe.manifest' }
foreach ($folder in 'turns', 'voice') {
    if (Test-Path -LiteralPath (Join-Path $tests $folder)) { $items += $folder }
}
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
& $tar -c --format=zip -f $zip -C $tests @items
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $zip)) {
    Write-Output "tar failed making $zip ($LASTEXITCODE)"
    Complete 2 $facts
}
$facts['made'] = $true
$facts['items'] = $items
$facts['exeMB'] = [math]::Round((Get-Item -LiteralPath $exe).Length / 1MB, 1)
$facts['zipMB'] = [math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)
Write-Output ("compiled the m5 harness in {0:0.0} s: m5_tests.exe {1} MB, m5-tests.zip {2} MB (cmark {3}, WebView2 {4})" -f `
    $facts['compileSeconds'], $facts['exeMB'], $facts['zipMB'], $cmark, $webView2)
Complete 0 $facts
