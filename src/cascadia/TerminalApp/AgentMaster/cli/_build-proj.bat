@echo off
REM Build a vcxproj in ISOLATION (a separate exe — never relinks the app exe (Agentmaster.exe), so no build
REM mutex needed). Validates a project as a real package binary. Arg %1 = vcxproj path (defaults to
REM agentmaster-cli.vcxproj).
setlocal
set "PROJ=%~1"
if "%PROJ%"=="" set "PROJ=K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\cli\agentmaster-cli.vcxproj"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
msbuild "%PROJ%" /m /p:Configuration=Debug /p:Platform=x64 ^
  /p:SolutionDir=K:\source\Agentmaster\ /p:OpenConsoleDir=K:\source\Agentmaster\ ^
  /v:m /clp:ErrorsOnly;Summary
exit /b %errorlevel%
