@echo off
rem Agentmaster: builds tools\stacksnap.exe (the non-suspending live stack sampler, see stacksnap.cpp).
rem Output: stacksnap.exe beside this script, or the compiler log on failure.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /W4 /std:c++17 stacksnap.cpp /Fe:stacksnap.exe /link dbghelp.lib psapi.lib 1>stacksnap_build.log 2>&1
if exist stacksnap.exe (echo BUILT_STACKSNAP) else (echo BUILD_FAILED & type stacksnap_build.log)
