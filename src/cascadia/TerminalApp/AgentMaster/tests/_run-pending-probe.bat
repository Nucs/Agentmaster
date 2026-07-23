@echo off
REM Agentmaster: build + run the ad-hoc OUT-OF-BAND pending-input probe (see pending_probe.cpp;
REM PENDING_INPUT.md §6 "LIVE" verification — the uia_probe precedent, standalone, NOT in the msbuild).
REM   %1 = a live claude.exe pid (resolve a session id -> pid via ~/.claude/sessions/<pid>.json)
REM   %2 = the report file (stdout dies on AttachConsole, so the report always goes to a file)
REM   %3 = optional tail-row count for the escaped fixture dump (default 60)
REM The probe links ONLY the two pure headers (PendingInput.h + PendingPaste.h) — no engine TU list to
REM keep in lockstep with run-m5-tests.bat.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /DUNICODE /D_UNICODE /Fe:pending_probe.exe pending_probe.cpp kernel32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
.\pending_probe.exe %1 %2 %3
echo [pending-probe] exit=%errorlevel% (0=box found, 1=no box/attach failed) — report: %2
