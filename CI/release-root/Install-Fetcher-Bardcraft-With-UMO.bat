@echo off
setlocal

set SCRIPT=%~dp0Install-Fetcher-Bardcraft-With-UMO.ps1
if not exist "%SCRIPT%" (
    echo Missing helper script:
    echo   %SCRIPT%
    echo.
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
set RESULT=%ERRORLEVEL%
echo.
if not "%RESULT%"=="0" (
    echo Bardcraft UMO install failed.
) else (
    echo Bardcraft UMO install finished.
)
echo.
pause
exit /b %RESULT%
