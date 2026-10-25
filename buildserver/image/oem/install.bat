@echo off
rem The build server's base image (buildserver/DESIGN.md §4.3): dockur copies this folder to C:\OEM and runs this file
rem once, as the auto-logon administrator, at the end of the unattended install. It only hands over to setup.ps1, whose
rem output goes to C:\OEM\setup.log so a failed step can be read later from the guest's console.
powershell -NoProfile -ExecutionPolicy Bypass -File C:\OEM\setup.ps1 > C:\OEM\setup.log 2>&1
