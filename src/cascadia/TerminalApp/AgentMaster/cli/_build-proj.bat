@echo off
REM Build agentmaster-cli.vcxproj in ISOLATION (a separate exe — never relinks WindowsTerminal.exe,
REM so no build mutex needed). Validates the project as a real package binary.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
msbuild K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\cli\agentmaster-cli.vcxproj ^
  /m /p:Configuration=Debug /p:Platform=x64 ^
  /p:SolutionDir=K:\source\Agentmaster\ /p:OpenConsoleDir=K:\source\Agentmaster\ ^
  /v:m /clp:ErrorsOnly;Summary
exit /b %errorlevel%
