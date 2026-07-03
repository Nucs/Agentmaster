@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set DIA=C:\Program Files\Microsoft Visual Studio\2022\Community\DIA SDK
cl /nologo /EHsc /O2 /I"%DIA%\include" dumpourscan.cpp /Fe:dumpourscan.exe /link "%DIA%\lib\amd64\diaguids.lib" dbghelp.lib ole32.lib oleaut32.lib advapi32.lib 1>dumpourscan_build.log 2>&1
if exist dumpourscan.exe (echo BUILT_OURSCAN) else (type dumpourscan_build.log)
