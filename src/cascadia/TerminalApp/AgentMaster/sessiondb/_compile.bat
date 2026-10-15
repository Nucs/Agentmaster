@echo off
REM Agentmaster session database service - standalone compile (the twin of agentmaster-sessiondb.vcxproj; no msbuild,
REM no PCH, no WinRT). Builds agentmaster-sessiondb.exe with duckdb.dll beside it into an output folder:
REM   _compile.bat [output folder] [x64|arm64]
REM The folder defaults to "out" beside this script (gitignored). The source list MUST stay in lockstep with the
REM vcxproj's ClCompile set. Prerequisites: the pinned libduckdb (pwsh -File tools\Get-DuckDb.ps1 -Arch x64) and
REM nlohmann/json from the vcpkg tree a full msbuild installs (obj\<arch>\vcpkg\<arch>-windows-static\include).
REM Flags: /W4 /WX /sdl /permissive- like the app; /O2 /MT (the static CRT, like the package); /utf-8 because the
REM engine sources are BOM-less UTF-8; /MP2 because a memory-loaded machine (a fleet of claude sessions) runs out of
REM commit with one compiler per core (raise it on an idle machine).
setlocal
set "ARCH=%~2"
if "%ARCH%"=="" set "ARCH=x64"
set "VCARCH=amd64"
if /i "%ARCH%"=="arm64" set "VCARCH=amd64_arm64"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
set "HERE=%~dp0"
set "AM=%HERE%.."
set "ROOT=%HERE%..\..\..\..\.."
set "DDB=%ROOT%\dep\duckdb\1.5.6\%ARCH%"
set "VCPKG=%ROOT%\obj\%ARCH%\vcpkg\%ARCH%-windows-static\include"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%out"
if not exist "%DDB%\duckdb.lib" (
  echo [error] libduckdb is missing from %DDB%: run pwsh -File tools\Get-DuckDb.ps1 -Arch %ARCH%
  exit /b 2
)
if not exist "%VCPKG%\nlohmann\json.hpp" (
  echo [error] nlohmann/json is missing from %VCPKG%: run one msbuild of the solution first (vcpkg installs it^)
  exit /b 2
)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /std:c++20 /EHsc /nologo /W4 /WX /permissive- /sdl /utf-8 /MP2 /O2 /MT /Zc:__cplusplus /Zc:inline /Zc:throwingNew ^
   /wd4201 /wd4312 /wd4467 /wd5105 /DUNICODE /D_UNICODE /D AGENTMASTER_DEV /I"%DDB%" /I"%VCPKG%" ^
   /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" /Fe:"%OUT%\agentmaster-sessiondb.exe" ^
   "%HERE%sessiondb_main.cpp" "%HERE%DuckDbWrap.cpp" "%HERE%SessionDbSchema.cpp" "%HERE%SessionDbExtract.cpp" "%HERE%SessionDbFold.cpp" ^
   "%HERE%SessionDbStore.cpp" "%HERE%SessionDbSummary.cpp" "%HERE%SessionDbIndex.cpp" "%HERE%SessionDbLog.cpp" "%HERE%SessionDbVerify.cpp" ^
   "%HERE%SessionDbSearch.cpp" "%HERE%SessionDbTools.cpp" "%HERE%SessionDbServer.cpp" "%HERE%SessionDbMcp.cpp" "%AM%\SessionDbClient.cpp" ^
   "%AM%\ClaudeSpawn.cpp" "%AM%\Persistence.cpp" "%AM%\ProcessInspect.cpp" "%AM%\ProcessInspect.Transcript.cpp" "%AM%\ProcessInspect.Content.cpp" ^
   "%AM%\ProcessInspect.Window.cpp" "%AM%\ProcessInspect.Summary.cpp" "%AM%\TranscriptStore.cpp" "%AM%\SessionStore.cpp" "%AM%\SessionSearch.cpp" ^
   "%AM%\ClaudeInvisibleChars.cpp" ^
   "%DDB%\duckdb.lib" ole32.lib user32.lib oleaut32.lib icu.lib shell32.lib advapi32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
copy /y "%DDB%\duckdb.dll" "%OUT%\" >nul
echo [ok] built %OUT%\agentmaster-sessiondb.exe
exit /b 0
