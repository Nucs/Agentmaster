@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild src\cascadia\TerminalApp\TerminalAppLib.vcxproj /t:ClCompile /p:SelectedFiles=Tab.cpp /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\ /nologo /v:m
