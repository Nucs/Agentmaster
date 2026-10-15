@echo off
REM SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
REM SPDX-License-Identifier: AGPL-3.0-or-later
REM
REM Builds bench_extract.exe (SESSIONS_DB.md section 17): bench_extract.cpp plus the session database service's
REM sources, the same list and flags as src\cascadia\TerminalApp\AgentMaster\sessiondb\_compile.bat without its main.
REM Objects and the exe go to the benchmarks' work folder (%AM_SESSIONDB_BENCH_DIR%\extractbench, else
REM %TEMP%\agentmaster-sessiondb-bench\extractbench), never into the repository. No app build, no package, no install.
REM Prerequisites as for _compile.bat: the pinned libduckdb (pwsh -File tools\Get-DuckDb.ps1 -Arch x64) and the vcpkg
REM nlohmann/json a full msbuild installs. Run the exe with AGENTMASTER_PROFILE set to a scratch folder: the engine
REM units log through the profile.
setlocal
if defined AM_SESSIONDB_BENCH_DIR (set "OUT=%AM_SESSIONDB_BENCH_DIR%\extractbench") else (set "OUT=%TEMP%\agentmaster-sessiondb-bench\extractbench")
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
set "ROOT=%~dp0..\.."
set "AM=%ROOT%\src\cascadia\TerminalApp\AgentMaster"
set "SDB=%AM%\sessiondb"
set "DDB=%ROOT%\dep\duckdb\1.5.6\x64"
set "VCPKG=%ROOT%\obj\x64\vcpkg\x64-windows-static\include"
if not exist "%DDB%\duckdb.lib" (
  echo [error] libduckdb is missing from %DDB%: run pwsh -File tools\Get-DuckDb.ps1 -Arch x64
  exit /b 2
)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /std:c++20 /EHsc /nologo /W4 /WX /permissive- /sdl /utf-8 /MP2 /O2 /MT /Zc:__cplusplus /Zc:inline /Zc:throwingNew ^
   /wd4201 /wd4312 /wd4467 /wd5105 /DUNICODE /D_UNICODE /D AGENTMASTER_DEV /I"%DDB%" /I"%VCPKG%" ^
   /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" /Fe:"%OUT%\bench_extract.exe" ^
   "%~dp0bench_extract.cpp" "%SDB%\DuckDbWrap.cpp" "%SDB%\SessionDbSchema.cpp" "%SDB%\SessionDbExtract.cpp" "%SDB%\SessionDbFold.cpp" ^
   "%SDB%\SessionDbStore.cpp" "%SDB%\SessionDbSummary.cpp" "%SDB%\SessionDbIndex.cpp" "%SDB%\SessionDbLog.cpp" "%SDB%\SessionDbVerify.cpp" ^
   "%SDB%\SessionDbSearch.cpp" "%SDB%\SessionDbTools.cpp" "%SDB%\SessionDbServer.cpp" "%SDB%\SessionDbMcp.cpp" "%AM%\SessionDbClient.cpp" ^
   "%AM%\ClaudeSpawn.cpp" "%AM%\Persistence.cpp" "%AM%\ProcessInspect.cpp" "%AM%\ProcessInspect.Transcript.cpp" "%AM%\ProcessInspect.Content.cpp" ^
   "%AM%\ProcessInspect.Window.cpp" "%AM%\ProcessInspect.Summary.cpp" "%AM%\TranscriptStore.cpp" "%AM%\SessionStore.cpp" "%AM%\SessionSearch.cpp" ^
   "%AM%\ClaudeInvisibleChars.cpp" ^
   "%DDB%\duckdb.lib" ole32.lib user32.lib oleaut32.lib icu.lib shell32.lib advapi32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
copy /y "%DDB%\duckdb.dll" "%OUT%\" >nul
echo [ok] built %OUT%\bench_extract.exe
exit /b 0
