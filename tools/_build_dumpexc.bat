@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 dumpexc.cpp /Fe:dumpexc.exe /link dbghelp.lib ole32.lib 1>dumpexc_build.log 2>&1
if exist dumpexc.exe (echo BUILT_DUMPEXC) else (type dumpexc_build.log)
