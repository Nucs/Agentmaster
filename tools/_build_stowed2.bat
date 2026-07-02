@echo off
rem Agentmaster: build the stowed-exception decoder (dumpstowed2) + the symbolized stack scanner
rem (scanstack). For MS-symbol-server symbolization, copy symsrv.dll (VS Remote Debugger x64) +
rem dbghelp.dll (System32) NEXT TO the built exes and pass a symbol path like
rem   srv*C:\symcache*https://msdl.microsoft.com/download/symbols;K:\source\Agentmaster\bin\x64\Debug\WindowsTerminal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /EHsc /W3 /O2 dumpstowed2.cpp /link dbghelp.lib
cl /nologo /EHsc /W3 /O2 scanstack.cpp /link dbghelp.lib
echo CL_EXIT=%ERRORLEVEL%
