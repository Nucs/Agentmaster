@echo off
REM Builds the CLI-dispatch probe (README.md) OUTSIDE the source tree: build.cmd [output folder]
REM (default Q:\tmp\cli-dispatch-probe). fakegui.exe compiles the REAL src\...\AgentMaster\CliDispatch.h at /W4 /WX
REM /permissive- plus the app's /we list, so a warning in the header fails here too. Nothing of the app is built.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=Q:\tmp\cli-dispatch-probe"
set "HERE=%~dp0"
set "HEADERS=%HERE%..\..\src\cascadia\TerminalApp\AgentMaster"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC
  exit /b 2
)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set FLAGS=/std:c++20 /EHsc /nologo /W4 /WX /permissive- /utf-8 /DUNICODE /D_UNICODE /we4189 /we4100 /we4242 /we4389 /we4244 /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\"
cl %FLAGS% /I"%HEADERS%" "%HERE%fakegui.cpp" /Fe:"%OUT%\fakegui.exe" /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
REM The fake CLI MUST be named agentmaster-cli.exe: CliDispatch.h runs the file of that name beside the GUI exe.
cl %FLAGS% "%HERE%fakecli.cpp" /Fe:"%OUT%\agentmaster-cli.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
cl %FLAGS% "%HERE%launcher.cpp" /Fe:"%OUT%\launcher.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
REM A second folder holds fakegui.exe WITHOUT a CLI beside it: the "CLI cannot be started" case.
if not exist "%OUT%\nocli" mkdir "%OUT%\nocli"
copy /y "%OUT%\fakegui.exe" "%OUT%\nocli\fakegui.exe" >nul
echo BUILD-OK %OUT%
