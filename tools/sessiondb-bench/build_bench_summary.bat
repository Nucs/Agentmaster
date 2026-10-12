@echo off
REM SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
REM SPDX-License-Identifier: AGPL-3.0-or-later
REM
REM Builds bench_summary.exe (SESSIONS_DB.md section 14): bench_summary.cpp plus the engine TUs the m5
REM harness links, optimized like a Release build (/O2 /DNDEBUG), so the baseline it measures is the
REM shipped code's speed. Objects and the exe go to the benchmarks' work folder
REM (%AM_SESSIONDB_BENCH_DIR%\cppbench, else %TEMP%\agentmaster-sessiondb-bench\cppbench), never into
REM the repository. No app build, no package, no install.
REM /MP4 caps the parallel compilers: this machine runs many claude sessions and free commit is limited.
REM shell32.lib: Engine.cpp's LaunchClaudeInstall calls ShellExecuteW (the harness gets it elsewhere).
setlocal
if defined AM_SESSIONDB_BENCH_DIR (set "OUT=%AM_SESSIONDB_BENCH_DIR%\cppbench") else (set "OUT=%TEMP%\agentmaster-sessiondb-bench\cppbench")
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set "AM=%~dp0..\..\src\cascadia\TerminalApp\AgentMaster"
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /MP4 /O2 /DNDEBUG /DUNICODE /D_UNICODE /I"%AM%" /Fo"%OUT%\obj\\" /Fe"%OUT%\bench_summary.exe" "%~dp0bench_summary.cpp" "%AM%\TranscriptTurns.cpp" "%AM%\SessionRegistry.cpp" "%AM%\HooksBridge.cpp" "%AM%\ClaudeSpawn.cpp" "%AM%\Persistence.cpp" "%AM%\SessionScanner.cpp" "%AM%\TranscriptWatcher.cpp" "%AM%\CommandWatch.cpp" "%AM%\ProcessInspect.cpp" "%AM%\ProcessInspect.Transcript.cpp" "%AM%\ProcessInspect.Content.cpp" "%AM%\ProcessInspect.Window.cpp" "%AM%\ProcessInspect.Summary.cpp" "%AM%\TranscriptStore.cpp" "%AM%\SessionSearch.cpp" "%AM%\SessionStore.cpp" "%AM%\Scheduler.cpp" "%AM%\Engine.cpp" "%AM%\ProcessObserver.cpp" "%AM%\ClaudeInvisibleChars.cpp" "%AM%\AgentHarnessCore.cpp" "%AM%\SessionSearchTools.cpp" ole32.lib user32.lib oleaut32.lib shell32.lib icu.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo built %OUT%\bench_summary.exe
exit /b 0
