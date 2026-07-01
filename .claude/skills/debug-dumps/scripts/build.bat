@echo off
setlocal enabledelayedexpansion
REM =====================================================================================================
REM Agentmaster debug-dumps skill — build every dump-analysis tool.
REM   Tools (DIA):     diasym dumpstack dumpourscan   (symbolize via msdia140.dll + DIA SDK diaguids.lib)
REM   Tools (dbghelp): dumpexc dumpwalk2 hangwalk     (only need dbghelp.lib)
REM   Fixture gen:     makedump                        (writes a small test dump; dbghelp.lib)
REM Run from anywhere: it cd's to its own dir. Re-run any time; it overwrites the .exe.
REM =====================================================================================================
cd /d "%~dp0"

REM --- locate VS 2022 (any edition) via vswhere; fall back to Community ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
)
if not defined VSDIR set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [debug-dumps] ERROR: vcvars64.bat not found under "%VSDIR%". Set VSDIR or install the C++ workload.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

REM --- locate the DIA SDK (ships with VS) ---
set "DIA=%VSDIR%\DIA SDK"
if not exist "%DIA%\include\dia2.h" (
  echo [debug-dumps] ERROR: DIA SDK not found at "%DIA%\include\dia2.h".
  echo               The DIA-based tools ^(diasym/dumpstack/dumpourscan^) need it; dbghelp tools still build.
)

set FAIL=0

echo(
echo [debug-dumps] building DIA tools (symbolization)...
for %%T in (diasym dumpstack dumpourscan) do (
  if exist "%DIA%\include\dia2.h" (
    cl /nologo /EHsc /O2 /std:c++17 /I"%DIA%\include" %%T.cpp /Fe:%%T.exe ^
       /link "%DIA%\lib\amd64\diaguids.lib" dbghelp.lib ole32.lib oleaut32.lib advapi32.lib 1>%%T_build.log 2>&1
    if exist %%T.exe ( echo   BUILT %%T.exe ) else ( echo   FAILED %%T ^(see scripts\%%T_build.log^) & set FAIL=1 )
  ) else (
    echo   SKIPPED %%T ^(no DIA SDK^)
  )
)

echo(
echo [debug-dumps] building dbghelp tools + the fixture generator...
REM makedump gets /Zi so makedump.pdb sits beside it -> the generated dump symbolizes makedump!FaultDeep.
cl /nologo /EHsc /O2 /Zi /std:c++17 makedump.cpp /Fe:makedump.exe /Fd:makedump.pdb /link dbghelp.lib 1>makedump_build.log 2>&1
if exist makedump.exe ( echo   BUILT makedump.exe ) else ( echo   FAILED makedump ^(see scripts\makedump_build.log^) & set FAIL=1 )
for %%T in (dumpexc dumpwalk2 hangwalk) do (
  cl /nologo /EHsc /O2 /std:c++17 %%T.cpp /Fe:%%T.exe /link dbghelp.lib 1>%%T_build.log 2>&1
  if exist %%T.exe ( echo   BUILT %%T.exe ) else ( echo   FAILED %%T ^(see scripts\%%T_build.log^) & set FAIL=1 )
)

echo(
if "%FAIL%"=="0" ( echo [debug-dumps] all tools built. ) else ( echo [debug-dumps] some tools FAILED ^(see *_build.log^). )
del /q *.obj 2>nul
exit /b %FAIL%
