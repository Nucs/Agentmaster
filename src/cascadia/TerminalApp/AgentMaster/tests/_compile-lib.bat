@echo off
REM Agentmaster: compile-check TerminalAppLib (static lib) without relinking the running exe.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
msbuild K:\source\Agentmaster\src\cascadia\TerminalApp\TerminalAppLib.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\ /v:m /clp:ErrorsOnly;Summary
exit /b %errorlevel%
