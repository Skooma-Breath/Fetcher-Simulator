@echo off
setlocal

set SCRIPT=%~dp0Apply-Fetcher-Public-Test-Config.ps1
if not exist "%SCRIPT%" (
    echo Missing helper script:
    echo   %SCRIPT%
    echo.
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
set RESULT=%ERRORLEVEL%
echo.
if not "%RESULT%"=="0" (
    echo Public test config update failed.
) else (
    echo Public test config update finished.
)
echo.
pause
exit /b %RESULT%
