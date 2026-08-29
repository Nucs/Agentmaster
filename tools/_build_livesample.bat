@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /std:c++17 livesample.cpp /Fe:livesample.exe /link dbghelp.lib psapi.lib user32.lib 1>livesample_build.log 2>&1
if exist livesample.exe (echo BUILT_LIVESAMPLE) else (echo BUILD_FAILED & type livesample_build.log)
