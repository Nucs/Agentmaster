@echo off
rem Agentmaster e2e: run provision.ps1 DETACHED from the desktop agent's run_command and log its output.
rem
rem Why a wrapper: the desktop agent's run_command waits for every process in its job and the MCP call gives up
rem after 300 s of silence, while provisioning downloads ~350 MB and extracts Git. The orchestrator therefore
rem starts this file with the agent's `launch` and polls C:\e2e\provision.log for the final "PROVISION " line
rem (and C:\e2e\provision.done for the exit code).
rem
rem Usage (in the guest):  Z:\e2e\src\guest\provision.cmd [provision.ps1 arguments...]
if not exist C:\e2e mkdir C:\e2e
del /q C:\e2e\provision.done 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0provision.ps1" %* > C:\e2e\provision.log 2>&1
rem The redirection comes FIRST: in "echo %ERRORLEVEL%> file" an exit code of 0-9 expands to "echo 0> file",
rem which cmd reads as a redirection of HANDLE 0 - the file is created EMPTY and the code is lost (seen
rem 2026-10-23: provision.done stayed 0 bytes after a provisioning that had succeeded).
> C:\e2e\provision.done echo %ERRORLEVEL%
