@echo off
rem Agentmaster: build tools\devidle.exe (the idle-GPU-device cost test behind
rem doc\agentmaster\RENDERING.md section 4b). Standalone: touches nothing in the app build tree.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /std:c++20 /W3 devidle.cpp /Fe:devidle.exe 1>devidle_build.log 2>&1
if exist devidle.exe (echo BUILT_DEVIDLE) else (echo BUILD_FAILED & type devidle_build.log)
