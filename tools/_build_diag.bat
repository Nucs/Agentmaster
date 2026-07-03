@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set DIA=C:\Program Files\Microsoft Visual Studio\2022\Community\DIA SDK
cl /nologo /EHsc /O2 /I"%DIA%\include" diasym.cpp /Fe:diasym.exe /link "%DIA%\lib\amd64\diaguids.lib" ole32.lib oleaut32.lib advapi32.lib 1>diasym_build.log 2>&1
if exist diasym.exe (echo BUILT_DIASYM) else (type diasym_build.log)
cl /nologo /EHsc /O2 /I"%DIA%\include" dumpstack.cpp /Fe:dumpstack.exe /link "%DIA%\lib\amd64\diaguids.lib" dbghelp.lib ole32.lib oleaut32.lib advapi32.lib 1>dumpstack_build.log 2>&1
if exist dumpstack.exe (echo BUILT_DUMPSTACK) else (type dumpstack_build.log)
