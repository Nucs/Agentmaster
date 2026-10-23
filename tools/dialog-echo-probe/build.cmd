@echo off
REM Builds the dialog-echo probe (README.md) OUTSIDE the source tree: build.cmd [output folder]
REM (default Q:\tmp\dialog-echo-probe). Compiles the REAL CallerSink.h / DialogEcho.h at /W4 /WX /permissive- plus the
REM app's /we list, and the REAL launcher shim (src\cascadia\wt\shim.cpp, with the repository's WIL) as shim\wtd.exe beside
REM a copy of the GUI stand-in named Agentmaster.exe: the shim forwards to the file of that name in its OWN folder. Never
REM run that copy by bare name (a bare name can find the installed Agentmaster on PATH). Nothing of the app is built.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=Q:\tmp\dialog-echo-probe"
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
if not exist "%OUT%\shim" mkdir "%OUT%\shim"
set FLAGS=/std:c++20 /EHsc /nologo /W4 /WX /permissive- /utf-8 /DUNICODE /D_UNICODE /we4189 /we4100 /we4242 /we4389 /we4244 /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\"
REM /MANIFEST:EMBED: the command-line linker writes the stand-in's Common Controls v6 manifest to a SEPARATE
REM dialoggui.exe.manifest by default, and the copy in shim\ (renamed Agentmaster.exe) then has none: it binds comctl32
REM v5.82 and dies at load with STATUS_ORDINAL_NOT_FOUND (0xC0000138, measured 2026-10-23) before writing a byte.
cl %FLAGS% /I"%HEADERS%" "%HERE%dialoggui.cpp" /Fe:"%OUT%\dialoggui.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED user32.lib
if errorlevel 1 exit /b 1
cl %FLAGS% "%HERE%launcher.cpp" /Fe:"%OUT%\launcher.exe" /link /SUBSYSTEM:CONSOLE user32.lib
if errorlevel 1 exit /b 1
REM The real shim: its relative includes (../TerminalApp/AgentMaster/...) resolve from its own folder.
cl %FLAGS% /I"%WIL%" "%REPO%\src\cascadia\wt\shim.cpp" /Fe:"%OUT%\shim\wtd.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
copy /y "%OUT%\dialoggui.exe" "%OUT%\shim\Agentmaster.exe" >nul
echo BUILD-OK %OUT%
