@echo off
rem Build server measurement image: dockur runs C:\OEM\install.bat once, at the end of the unattended install.
rem It only hands over to setup.ps1, which registers the measurement job runner (agent.ps1) and records the
rem install's end time. Output goes to C:\OEM\setup.log so a failed step can be read later from the guest.
powershell -NoProfile -ExecutionPolicy Bypass -File C:\OEM\setup.ps1 > C:\OEM\setup.log 2>&1
