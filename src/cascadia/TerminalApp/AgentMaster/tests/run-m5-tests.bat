@echo off
REM Agentmaster M5 standalone test runner. Sets up MSVC, compiles the themed test TUs + the
REM engine TUs (no PCH/WinRT), and runs the suite. Exit code 0 == all pass.
REM  /utf-8 - matches the msbuild app build (common.build.pre.props adds it globally): every
REM           engine source is BOM-less UTF-8, so WITHOUT the flag cl reads them in the ANSI
REM           codepage and wide literals holding non-ASCII (the SessionSearch snippet ellipsis,
REM           its scope emoji prefixes, ...) compile to DIFFERENT constants than the shipped
REM           binary carries (the C4066 "characters beyond first" warning was the tell).
REM  /MP    - parallel compile across the ~20 TUs (measured ~3x on this suite).
REM  /DUNICODE /D_UNICODE - app-build parity too: msbuild defines both globally, and Updater.h's
REM           resource lookup (RT_RCDATA -> MAKEINTRESOURCE) requires the W flavor.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
REM user32 + oleaut32: ProcessInspect's BringClaudeWindowToFront (EnumWindows/ShowWindow/... + UIA BSTR names)
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /MP /DUNICODE /D_UNICODE /Fe:m5_tests.exe m5_tests.cpp tests_state.cpp tests_spawn_sched.cpp tests_persistence.cpp tests_transcript.cpp tests_summary_anchor.cpp tests_commands.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ..\SessionScanner.cpp ..\CommandWatch.cpp ..\ProcessInspect.cpp ..\ProcessInspect.Transcript.cpp ..\ProcessInspect.Content.cpp ..\ProcessInspect.Window.cpp ..\ProcessInspect.Summary.cpp ..\TranscriptStore.cpp ..\SessionSearch.cpp ..\SessionStore.cpp ..\Scheduler.cpp ..\Engine.cpp ..\ProcessObserver.cpp ole32.lib user32.lib oleaut32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
REM State isolation: run against a WIPED %TEMP% scratch profile so the engine traces the linked
REM units emit ([fork-echo]/[send]/[recon-*] -> hooks/autorunner/scanner logs) and the window-record
REM disk round-trips (windows\<id>.json + open-windows.json) never touch the LIVE ~/.agentmaster —
REM the RELEASE install's active profile. m5_tests.exe self-defaults to this same scratch dir when
REM the var is absent (a direct exe run); only the bat wipes it fresh.
set "AGENTMASTER_PROFILE=%TEMP%\agentmaster-m5-tests"
rd /s /q "%TEMP%\agentmaster-m5-tests" >nul 2>&1
REM Explicit ".\" so it launches even when NoDefaultCurrentDirectoryInExePath is set.
.\m5_tests.exe
exit /b %errorlevel%
