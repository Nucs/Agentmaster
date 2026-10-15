@echo off
REM Agentmaster - build and run the LIVE test of the agent harness (harness_live.cpp, SDK_HARNESS.md section 8).
REM
REM It starts the real claude.exe and makes real model calls: a logged-in Claude Code and a network are needed,
REM and each run uses a little of the account's quota. It is NOT part of run-m5-tests.bat.
REM
REM   run-harness-live.bat [arguments for harness_live.exe]
REM   e.g.  run-harness-live.bat --agent "the session where I fixed the tab drag crash"
REM         run-harness-live.bat --only-agent --agent "sessions about the taskbar badge"
REM
REM Environment variables it reads:
REM   AM_NLOHMANN_INCLUDE  a folder that holds nlohmann\json.hpp. Needed only until a full app build has installed the
REM                        vcpkg port nlohmann-json (then obj\x64\vcpkg\x64-windows-static\include has it).
REM   AM_LIVE_OUT          where the object files and the exe go. Default: %TEMP%\agentmaster-harness-live-build.
REM                        (Not the source tree: K: is nearly full, and a build here must not touch the app's outputs.)
REM   AM_LIVE_NO_AGENT     set to 1 to build the harness scenarios alone, without the Sessions search agent.
REM   AM_LIVE_PROFILE      the profile the test runs in. Default: %TEMP%\agentmaster-harness-live-profile, so test
REM                        traces never go into a real profile's hooks.log. An inherited AGENTMASTER_PROFILE is
REM                        IGNORED on purpose: a shell hosted by a running Agentmaster carries that instance's
REM                        live profile. The search-agent scenario builds its own session index in the test profile
REM                        on its first run (a full parse of the window's transcripts: slow once, then cached).
REM
REM Exit code: 0 = every check passed; 1 = the build failed; 2 = a prerequisite is missing; else the failed checks.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
set "AM_TESTS=%~dp0"
REM %~dp0 = <repo>\src\cascadia\TerminalApp\AgentMaster\tests\ : the repository root is five folders up.
for %%I in ("%AM_TESTS%..\..\..\..\..") do set "AM_REPO=%%~fI"
set "AM_SDK=%AM_REPO%\refs\claude-agent-sdk-cpp"
if not exist "%AM_SDK%\src\client.cpp" (
  echo [error] the SDK submodule is not checked out. Run:  git submodule update --init refs/claude-agent-sdk-cpp
  exit /b 2
)
set "AM_VCPKG=%AM_REPO%\obj\x64\vcpkg\x64-windows-static"
set "AM_JSON_INC=%AM_VCPKG%\include"
if not exist "%AM_JSON_INC%\nlohmann\json.hpp" set "AM_JSON_INC=%AM_NLOHMANN_INCLUDE%"
if not exist "%AM_JSON_INC%\nlohmann\json.hpp" (
  echo [error] nlohmann\json.hpp was not found. Build the app once ^(vcpkg installs nlohmann-json^), or set AM_NLOHMANN_INCLUDE
  echo         to a folder that holds nlohmann\json.hpp.
  exit /b 2
)
if "%AM_LIVE_OUT%"=="" set "AM_LIVE_OUT=%TEMP%\agentmaster-harness-live-build"
if not exist "%AM_LIVE_OUT%" mkdir "%AM_LIVE_OUT%"
set "AM_AGENT_SRC=..\SessionSearchAgent.cpp ..\SessionSearchTools.cpp"
set "AM_AGENT_FLAGS="
if "%AM_LIVE_NO_AGENT%"=="1" (
  set "AM_AGENT_SRC="
  set "AM_AGENT_FLAGS=/DAM_LIVE_NO_AGENT"
)
cd /d "%AM_TESTS%"
REM The same switches as run-m5-tests.bat (/utf-8, UNICODE: parity with the app build), plus the SDK's include folders
REM and /O2: this test PRINTS latencies (process start, time to the first result, the index read), and an unoptimized
REM build reads the session index several times slower than the Release app does.
REM The engine list is m5's: the search agent reads the session index, the session store and the registry.
cl /std:c++20 /EHsc /nologo /W3 /O2 /utf-8 /MP /DUNICODE /D_UNICODE %AM_AGENT_FLAGS% /I"%AM_SDK%\include" /I"%AM_SDK%\src" /I"%AM_JSON_INC%" /Fo"%AM_LIVE_OUT%\\" /Fd"%AM_LIVE_OUT%\harness_live.pdb" /Fe:"%AM_LIVE_OUT%\harness_live.exe" harness_live.cpp ..\AgentHarness.cpp ..\AgentHarnessCore.cpp ..\ClaudeSdkUnity.cpp ..\ClaudeSdkProcessWin32.cpp %AM_AGENT_SRC% ..\TranscriptTurns.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ..\SessionScanner.cpp ..\TranscriptWatcher.cpp ..\CommandWatch.cpp ..\ProcessInspect.cpp ..\ProcessInspect.Transcript.cpp ..\ProcessInspect.Content.cpp ..\ProcessInspect.Window.cpp ..\ProcessInspect.Summary.cpp ..\TranscriptStore.cpp ..\SessionSearch.cpp ..\SessionStore.cpp ..\Scheduler.cpp ..\Engine.cpp ..\SessionDbRefresher.cpp ..\SessionDbClient.cpp ..\ProcessObserver.cpp ..\ClaudeInvisibleChars.cpp ole32.lib user32.lib oleaut32.lib icu.lib shell32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
set "AGENTMASTER_PROFILE=%TEMP%\agentmaster-harness-live-profile"
if not "%AM_LIVE_PROFILE%"=="" set "AGENTMASTER_PROFILE=%AM_LIVE_PROFILE%"
if not exist "%AGENTMASTER_PROFILE%" mkdir "%AGENTMASTER_PROFILE%"
"%AM_LIVE_OUT%\harness_live.exe" %*
exit /b %errorlevel%
