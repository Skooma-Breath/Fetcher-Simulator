@echo off
setlocal

set "SCRIPT=%~dp0Apply-Fetcher-Public-Test-Config.ps1"
if exist "%SCRIPT%" goto run_config

echo Missing helper script:
echo   %SCRIPT%
echo.
pause
exit /b 1

:run_config
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
set "RESULT=%ERRORLEVEL%"
echo.
if "%RESULT%"=="0" goto config_succeeded
echo Public test config update failed.
goto finish

:config_succeeded
echo Public test config update finished.

:finish
echo.
pause
exit /b %RESULT%
