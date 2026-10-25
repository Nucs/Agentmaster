<#
.SYNOPSIS
    Measurement: compiles and links Agentmaster's engine test harness (m5_tests.exe, 63 C++20 translation units with
    no PCH) from a scratch copy of the sources, timed, so a build-server VM and the host can be compared on real
    compiler work.

.DESCRIPTION
    Runs, through cmd.exe, the given Visual C++ environment script (vcvars64.bat) and then the harness's own cl
    command line from tests\run-m5-tests.bat, with two changes: /MP<n> instead of /MP (the same parallelism on both
    machines) and no cmark (the transcript overlay's Markdown tests need a vcpkg tree the copy does not have). It works
    in a copy of src\cascadia\TerminalApp\AgentMaster exported with git archive, never in the repository, so the
    objects it writes land in that copy. Writes JSON: the wall time, the exit code, the number of objects made, and
    the compiler's version line.

.PARAMETER SourceDir
    The copy's tests folder (...\AgentMaster\tests); its .obj and .exe files are deleted first.

.PARAMETER VcVars
    The path of vcvars64.bat (VS 2022 Community on the host, Build Tools in a VM).

.PARAMETER Parallel
    The /MP value: how many cl processes compile at once.

.PARAMETER OutFile
    Where the JSON result goes.

.OUTPUTS
    None; the JSON goes to -OutFile.

.NOTES
    A compile error ends the run with cl's exit code in the JSON and the last 40 lines of its output.
#>
param(
    [Parameter(Mandatory)] [string]$SourceDir,
    [Parameter(Mandatory)] [string]$VcVars,
    [int]$Parallel = 4,
    [Parameter(Mandatory)] [string]$OutFile
)
$ErrorActionPreference = 'Stop'
Get-ChildItem $SourceDir -Include '*.obj', '*.exe', '*.pdb', '*.ilk' -File -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force
$tests = 'm5_tests.cpp tests_state.cpp tests_spawn_sched.cpp tests_persistence.cpp tests_transcript.cpp tests_summary_anchor.cpp ' +
    'tests_commands.cpp tests_tabdrag.cpp tests_invisible.cpp tests_taskbar.cpp tests_turns.cpp tests_markdown.cpp ' +
    'tests_sidepanel.cpp tests_presets.cpp tests_harness.cpp tests_sessiondb.cpp tests_messaging.cpp tests_messaging_search.cpp ' +
    'tests_messaging_directory.cpp tests_messaging_newthread.cpp tests_messaging_card.cpp tests_messaging_taps.cpp ' +
    'tests_messaging_steer.cpp tests_messaging_files.cpp tests_tuimenus.cpp tests_ask.cpp tests_rewind.cpp tests_restorewake.cpp ' +
    'tests_tabtools.cpp'
$engine = 'ApiErrorRewind TranscriptTurns SessionRegistry HooksBridge ClaudeSpawn Persistence SessionScanner TranscriptWatcher ' +
    'CommandWatch ProcessInspect ProcessInspect.Transcript ProcessInspect.Content ProcessInspect.Window ProcessInspect.Summary ' +
    'TranscriptStore SessionSearch SessionStore Scheduler Engine SessionDbRefresher SessionDbClient Messaging Messaging.Inbound ' +
    'Messaging.Questions Messaging.Menus Messaging.Files MessagingFileServer TelegramApi ScreenImage ProcessObserver ' +
    'ClaudeInvisibleChars AgentHarnessCore SessionSearchTools'
$engineFiles = ($engine.Split(' ') | ForEach-Object { "..\$_.cpp" }) -join ' '
$cl = "cl /std:c++20 /EHsc /nologo /W3 /utf-8 /MP$Parallel /DUNICODE /D_UNICODE /Fe:m5_tests.exe $tests $engineFiles " +
    'ole32.lib user32.lib oleaut32.lib icu.lib'
$script = "@echo off`r`ncall `"$VcVars`" >nul 2>&1 || exit /b 90`r`ncd /d `"$SourceDir`"`r`n$cl`r`n"
$cmdFile = Join-Path $env:TEMP 'compile-bench.cmd'
Set-Content -Encoding ascii -Path $cmdFile -Value $script
$version = (& cmd.exe /c "call `"$VcVars`" >nul 2>&1 && cl 2>&1" | Select-Object -First 1)
$watch = [Diagnostics.Stopwatch]::StartNew()
$output = & cmd.exe /c $cmdFile 2>&1
$code = $LASTEXITCODE
$seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
[ordered]@{
    computer     = $env:COMPUTERNAME
    logicalCpus  = [Environment]::ProcessorCount
    parallel     = $Parallel
    compiler     = "$version"
    exitCode     = $code
    seconds      = $seconds
    objects      = @(Get-ChildItem $SourceDir -Filter '*.obj' -File).Count
    exeMB        = if (Test-Path (Join-Path $SourceDir 'm5_tests.exe')) { [math]::Round((Get-Item (Join-Path $SourceDir 'm5_tests.exe')).Length / 1MB, 1) } else { $null }
    outputTail   = if ($code -ne 0) { @($output | Select-Object -Last 40 | ForEach-Object { "$_" }) } else { @() }
} | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 $OutFile
