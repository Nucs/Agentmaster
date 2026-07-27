@echo off
REM Agentmaster: build + run the ad-hoc TAB-TOOLTIP wheel-scroll probe (see tooltip_probe.cpp).
REM %1 = seconds to watch (default 25). Read-only: it watches the UI Automation tree and never
REM touches the mouse -- YOU hover the tab and spin the wheel while it prints the timeline.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
REM Standalone: pure UIA + Win32, no engine TUs (unlike uia_probe.bat) -- so it compiles in seconds.
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /DUNICODE /D_UNICODE /Fe:tooltip_probe.exe tooltip_probe.cpp ole32.lib oleaut32.lib user32.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
echo.
.\tooltip_probe.exe %1 %2
exit /b %errorlevel%
