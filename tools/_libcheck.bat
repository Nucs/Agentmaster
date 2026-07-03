@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild src\cascadia\TerminalApp\TerminalAppLib.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\ /clp:NoSummary;ErrorsOnly /nologo /v:m
