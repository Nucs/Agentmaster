@echo off
REM Agentmaster M5 standalone test runner. Sets up MSVC, compiles the unity test TU
REM (no PCH/WinRT), and runs it. Exit code 0 == all pass.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
REM user32 + oleaut32: ProcessInspect's BringClaudeWindowToFront (EnumWindows/ShowWindow/... + UIA BSTR names)
cl /std:c++20 /EHsc /nologo /W3 /Fe:m5_tests.exe m5_tests.cpp tests_state.cpp tests_spawn_sched.cpp tests_persistence.cpp tests_transcript.cpp tests_summary_anchor.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ..\SessionScanner.cpp ..\ProcessInspect.cpp ..\ProcessInspect.Transcript.cpp ..\ProcessInspect.Content.cpp ..\ProcessInspect.Window.cpp ..\ProcessInspect.Summary.cpp ..\TranscriptStore.cpp ..\SessionSearch.cpp ..\SessionStore.cpp ..\Scheduler.cpp ole32.lib user32.lib oleaut32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
REM Explicit ".\" so it launches even when NoDefaultCurrentDirectoryInExePath is set.
.\m5_tests.exe
exit /b %errorlevel%
