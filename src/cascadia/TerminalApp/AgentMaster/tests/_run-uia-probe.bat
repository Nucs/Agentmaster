@echo off
REM Agentmaster: build + run the ad-hoc UIA tab-name probe (see uia_probe.cpp). %1 %2 = optional
REM titleHint / cwdLeaf passed through to ScoreClaudeTabName.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
cl /std:c++20 /EHsc /nologo /W3 /Fe:uia_probe.exe uia_probe.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ..\SessionScanner.cpp ..\ProcessInspect.cpp ole32.lib user32.lib oleaut32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
.\uia_probe.exe %1 %2
exit /b %errorlevel%
