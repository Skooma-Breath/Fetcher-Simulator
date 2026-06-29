@echo off
setlocal

set "ROOT=%~dp0"
set "SCRIPT=%ROOT%Update-Fetcher-Simulator.ps1"
if exist "%SCRIPT%" goto run_updater

echo Missing updater script:
echo   %SCRIPT%
echo.
pause
exit /b 1

:run_updater
set "TEMP_SCRIPT=%TEMP%\Fetcher-Simulator-Updater-%RANDOM%-%RANDOM%.ps1"
copy /y "%SCRIPT%" "%TEMP_SCRIPT%" >nul
if errorlevel 1 goto copy_failed

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TEMP_SCRIPT%" -InstallRoot "%ROOT%" %*
set "RESULT=%ERRORLEVEL%"
del /q "%TEMP_SCRIPT%" >nul 2>&1
echo.
if "%RESULT%"=="0" goto update_succeeded
echo Fetcher Simulator update failed.
goto finish

:copy_failed
echo Could not stage the updater script in:
echo   %TEMP%
set "RESULT=1"
goto finish

:update_succeeded
echo Fetcher Simulator is up to date.

:finish
echo.
pause
exit /b %RESULT%
