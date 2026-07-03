@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 dumpwalk2.cpp /Fe:dumpwalk2.exe /link dbghelp.lib 1>dumpwalk2_build.log 2>&1
if exist dumpwalk2.exe (echo BUILT_DUMPWALK2) else (type dumpwalk2_build.log)
cl /nologo /EHsc /O2 hangwalk.cpp /Fe:hangwalk.exe /link dbghelp.lib 1>hangwalk_build.log 2>&1
if exist hangwalk.exe (echo BUILT_HANGWALK) else (type hangwalk_build.log)
