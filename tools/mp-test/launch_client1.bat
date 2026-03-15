@echo off
setlocal

rem ============================================================
rem  Client 1  —  main monitor, 2560x1440, position (0,0)
rem  Player: test1
rem
rem  Script lives at openmw\tools\mp-test\
rem  %ROOT% = this dir, so:
rem    openmw.exe  = %ROOT%..\..\MSVC2022_64\RelWithDebInfo\openmw.exe
rem    mp-clients\ = %ROOT%..\..\..\mp-clients\
rem ============================================================

set ROOT=%~dp0
set CLIENT_DIR=%ROOT%..\..\..\mp-clients\client1
set OPENMW=%ROOT%..\..\MSVC2022_64\RelWithDebInfo\openmw.exe
set REAL_USER_CFG=C:\Users\REPTILE\Documents\My Games\OpenMW

if not exist "%CLIENT_DIR%" mkdir "%CLIENT_DIR%"

if not exist "%CLIENT_DIR%\openmw.cfg" (
    echo # Client 1 local overrides> "%CLIENT_DIR%\openmw.cfg"
)

(
    echo [Video]
    echo resolution x = 2560
    echo resolution y = 1440
    echo window mode = 2
    echo window border = true
    echo window x = 0
    echo window y = 0
) > "%CLIENT_DIR%\settings.cfg"

"%OPENMW%" ^
    --config    "%REAL_USER_CFG%" ^
    --config    "%CLIENT_DIR%" ^
    --user-data "%CLIENT_DIR%" ^
    --log-dir   "%CLIENT_DIR%" ^
    --connect   127.0.0.1:25565 ^
    --mp-name   test1

endlocal
