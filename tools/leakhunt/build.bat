@echo off
setlocal
REM =====================================================================================================
REM Agentmaster leak hunt - build the two native tools beside this file.
REM   snapdump.exe   - a minidump of a RUNNING process from a PSS snapshot (dbghelp + psapi)
REM   heapcensus.exe - count a full dump's objects by vftable, names from the PDBs (DIA SDK + dbghelp)
REM Run from anywhere: it cd's to its own folder. VS 2022 is located with vswhere (falls back to
REM Community); msdia140.dll is loaded at run time from the Community path in heapcensus.cpp (kMsdia).
REM =====================================================================================================
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
)
if not defined VSDIR set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "DIA=%VSDIR%\DIA SDK"
set FAIL=0
cl /nologo /EHsc /O2 /W4 /std:c++20 snapdump.cpp /Fe:snapdump.exe /link dbghelp.lib psapi.lib 1>snapdump_build.log 2>&1
if exist snapdump.exe ( echo   BUILT snapdump.exe ) else ( echo   FAILED snapdump ^(see snapdump_build.log^) & set FAIL=1 )
cl /nologo /EHsc /O2 /W4 /std:c++20 /I"%DIA%\include" heapcensus.cpp /Fe:heapcensus.exe /link "%DIA%\lib\amd64\diaguids.lib" dbghelp.lib ole32.lib oleaut32.lib advapi32.lib 1>heapcensus_build.log 2>&1
if exist heapcensus.exe ( echo   BUILT heapcensus.exe ) else ( echo   FAILED heapcensus ^(see heapcensus_build.log^) & set FAIL=1 )
del /q *.obj 2>nul
exit /b %FAIL%
