@echo off
rem Runs Setup.ps1, which installs, updates or removes GPU vrad3 in Half-Life: Alyx.
setlocal
if not exist "%~dp0Setup.ps1" (
    echo Setup.ps1 is missing. Put Setup.cmd and Setup.ps1 in the same folder as the other files.
    pause
    exit /b 1
)
set "PSModulePath="
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup.ps1" %*
