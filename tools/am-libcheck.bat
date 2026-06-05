@echo off
REM Agentmaster: compile-check the TerminalApp static lib (no exe relink, no lock needed).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
msbuild K:\source\Agentmaster\src\cascadia\TerminalApp\TerminalAppLib.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\ /v:m /clp:Summary;ErrorsOnly
exit /b %errorlevel%
