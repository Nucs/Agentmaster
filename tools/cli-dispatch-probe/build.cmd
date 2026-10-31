@echo off
REM Builds the CLI-dispatch probe (README.md) OUTSIDE the source tree: build.cmd [output folder]
REM (default Q:\tmp\cli-dispatch-probe). fakegui.exe compiles the REAL src\...\AgentMaster\CliDispatch.h at /W4 /WX
REM /permissive- plus the app's /we list, so a warning in the header fails here too. The REAL launcher shim
REM (src\cascadia\wt\shim.cpp, with the repository's WIL) goes to shim\wtd.exe, beside copies of the stand-ins named as
REM the shim expects them: shim\Agentmaster.exe (fakegui) and shim\agentmaster-cli.exe (fakecli). Never run that
REM Agentmaster.exe by bare name (a bare name can find the installed Agentmaster on PATH). Nothing of the app is built.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=Q:\tmp\cli-dispatch-probe"
set "HERE=%~dp0"
set "REPO=%HERE%..\.."
set "HEADERS=%REPO%\src\cascadia\TerminalApp\AgentMaster"
set "WIL="
for /d %%W in ("%REPO%\packages\Microsoft.Windows.ImplementationLibrary.*") do set "WIL=%%W\include"
if "%WIL%"=="" (
  echo [error] no WIL package under %REPO%\packages - restore the packages first
  exit /b 2
)
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
REM The real shim: its relative includes (../TerminalApp/AgentMaster/...) resolve from its own folder. It runs the
REM agentmaster-cli.exe and forwards to the Agentmaster.exe in its OWN folder, so the stand-ins are copied there.
if not exist "%OUT%\shim" mkdir "%OUT%\shim"
cl %FLAGS% /I"%WIL%" "%REPO%\src\cascadia\wt\shim.cpp" /Fe:"%OUT%\shim\wtd.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
copy /y "%OUT%\fakegui.exe" "%OUT%\shim\Agentmaster.exe" >nul
copy /y "%OUT%\agentmaster-cli.exe" "%OUT%\shim\agentmaster-cli.exe" >nul
echo BUILD-OK %OUT%
