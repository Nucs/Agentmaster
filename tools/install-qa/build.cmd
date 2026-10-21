@echo off
rem Build the install QA probe (amprobe.cpp) as Agentmaster.exe.
rem   build.cmd [output folder]   (default Q:\tmp\amprobe - builds write to Q:, never into the source tree)
rem Static CRT (/MT): a test desktop has no Visual C++ runtime. The manifest is embedded (/MANIFEST:EMBED): the
rem comctl32 v6 dependency must travel INSIDE the exe when an install copies it. Not part of the product build.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=Q:\tmp\amprobe"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC
  exit /b 2
)
if not exist "%OUT%" mkdir "%OUT%"
rem rc resolves the RCDATA path relative to the current folder: run it from here.
pushd "%~dp0"
rc /nologo /fo "%OUT%\amprobe.res" amprobe.rc
set RC_EXIT=%ERRORLEVEL%
popd
if not "%RC_EXIT%"=="0" exit /b 1
cd /d "%OUT%"
cl /nologo /std:c++20 /EHsc /O1 /MT /W4 /utf-8 /DUNICODE /D_UNICODE /I "%~dp0..\..\src\cascadia\TerminalApp\AgentMaster" "%~dp0amprobe.cpp" amprobe.res /Fe:Agentmaster.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED winhttp.lib comctl32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib user32.lib advapi32.lib wtsapi32.lib propsys.lib
exit /b %ERRORLEVEL%
