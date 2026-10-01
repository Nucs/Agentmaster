@echo off
rem Agentmaster: build tools\warpbench.exe (the CPU-vs-GPU terminal frame benchmark behind
rem doc\agentmaster\RENDERING.md). Standalone: touches nothing in the app build tree.
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /std:c++20 /W3 warpbench.cpp /Fe:warpbench.exe 1>warpbench_build.log 2>&1
if exist warpbench.exe (echo BUILT_WARPBENCH) else (echo BUILD_FAILED & type warpbench_build.log)
