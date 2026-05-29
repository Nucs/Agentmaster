@echo off
REM Agentmaster M5 standalone test runner. Sets up MSVC, compiles the unity test TU
REM (no PCH/WinRT), and runs it. Exit code 0 == all pass.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
cl /std:c++20 /EHsc /nologo /W3 /Fe:m5_tests.exe m5_tests.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ole32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
m5_tests.exe
exit /b %errorlevel%
