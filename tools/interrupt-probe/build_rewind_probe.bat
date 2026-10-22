@echo off
REM Agentmaster - builds rewind_probe.exe (tools\interrupt-probe\README.md): the REAL planner / rewind /
REM classifier of ApiErrorRewind.cpp, linked from the objects of a built m5 harness, so cpp_probe.py can run
REM them on transcripts a real claude.exe wrote.
REM
REM   build_rewind_probe.bat <harness obj folder> <output folder>
REM     <harness obj folder>  the folder where run-m5-tests.bat (or an isolated copy of it) compiled - it holds
REM                           ApiErrorRewind.obj and the other engine objects. They must be built from the SAME
REM                           sources as this checkout's ApiErrorRewind.h, or the link fails or the probe lies.
REM     <output folder>       where rewind_probe.obj / .exe go (a folder on Q:, never the source tree).
REM
REM Links every engine object of the harness except m5_tests.obj, the tests_*.obj (each holds a test's own
REM entry points) and MarkdownBlocks.obj (it needs cmark.lib, and nothing here uses it). The compile flags
REM are the harness's (/W3, /utf-8, UNICODE, the static CRT), so the objects and the probe agree.
REM Exit code: 0 = built, 1 = cl failed, 2 = a usage or environment error.
setlocal EnableDelayedExpansion
if "%~2"=="" (
  echo usage: build_rewind_probe.bat ^<harness obj folder^> ^<output folder^>
  exit /b 2
)
set "HARNESS=%~f1"
set "OUT=%~f2"
set "KIT=%~dp0"
set "ENGINE=%KIT%..\..\src\cascadia\TerminalApp\AgentMaster"
if not exist "%HARNESS%\ApiErrorRewind.obj" (
  echo [error] no ApiErrorRewind.obj in "%HARNESS%" - build the m5 harness first
  exit /b 2
)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
if not exist "%OUT%" mkdir "%OUT%"
REM One loop, no labels: delayed expansion reads each object's name inside the loop body.
set "OBJS="
for %%f in ("%HARNESS%\*.obj") do (
  set "N=%%~nf"
  set "SKIP="
  if /i "!N!"=="m5_tests" set "SKIP=1"
  if /i "!N!"=="MarkdownBlocks" set "SKIP=1"
  if /i "!N:~0,6!"=="tests_" set "SKIP=1"
  if not defined SKIP set OBJS=!OBJS! "%%~ff"
)
cd /d "%OUT%"
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /DUNICODE /D_UNICODE /I"%ENGINE%" /Fe:rewind_probe.exe "%KIT%rewind_probe.cpp" !OBJS! ole32.lib user32.lib oleaut32.lib icu.lib shell32.lib
if errorlevel 1 exit /b 1
exit /b 0
