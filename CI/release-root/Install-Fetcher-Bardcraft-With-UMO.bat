@echo off
setlocal

set "SCRIPT=%~dp0Install-Fetcher-Bardcraft-With-UMO.ps1"
if exist "%SCRIPT%" goto run_installer

echo Missing helper script:
echo   %SCRIPT%
echo.
pause
exit /b 1

:run_installer
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" goto install_succeeded
echo Bardcraft UMO install failed.
goto finish

:install_succeeded
echo Bardcraft UMO install finished.

:finish
echo.
pause
exit /b %RESULT%
