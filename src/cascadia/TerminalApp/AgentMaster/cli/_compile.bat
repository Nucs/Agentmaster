@echo off
REM Agentmaster CLI standalone compile (the tests/ harness pattern — no PCH/WinRT/TerminalAppLib).
REM Produces agentcli.exe in this dir. Validates all engine-API usage without relinking the app.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\cli"
REM /D AGENTMASTER_DEV: this standalone build represents the DEV CLI — when run UNPACKAGED and
REM outside any app (no inherited AGENTMASTER_PROFILE), it defaults to ~/.agentmaster-dev.
cl /std:c++20 /EHsc /nologo /W3 /D AGENTMASTER_DEV /Fe:agentcli.exe agentcli.cpp ^
   ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ^
   ..\SessionScanner.cpp ..\ProcessInspect.cpp ..\TranscriptStore.cpp ..\SessionSearch.cpp ^
   ole32.lib user32.lib oleaut32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo [ok] built agentcli.exe
exit /b 0
