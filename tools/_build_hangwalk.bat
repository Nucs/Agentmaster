@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 hangwalk.cpp /Fe:hangwalk.exe /link dbghelp.lib 1>hangwalk_build.log 2>&1
if exist hangwalk.exe (echo BUILT_HANGWALK) else (echo BUILD_FAILED & type hangwalk_build.log)
