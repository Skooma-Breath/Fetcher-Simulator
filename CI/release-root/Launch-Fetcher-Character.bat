@echo off
setlocal

set "ROOT=%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%Launch-Fetcher-Character.ps1" -InstallRoot "%ROOT%."
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
    echo.
    echo Fetcher Simulator launch failed.
    pause
)

exit /b %EXIT_CODE%
