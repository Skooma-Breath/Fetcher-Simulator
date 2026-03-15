@echo off
setlocal

rem ============================================================
rem  Client 2  —  bottom-right of monitor 2 (LEFT of primary)
rem  Both monitors 2560x1440, window 960x540
rem  Player: turdface
rem
rem  Monitor 2 desktop coords: x = -2560 to 0, y = 0 to 1440
rem  Bottom-right of a 960x540 window:  x=-960, y=900
rem
rem  Adjust WIN_X / WIN_Y if your layout differs.
set WIN_X=-960
set WIN_Y=900
rem ============================================================

set ROOT=%~dp0
set CLIENT_DIR=%ROOT%..\..\..\mp-clients\client2
set OPENMW=%ROOT%..\..\MSVC2022_64\RelWithDebInfo\openmw.exe
set REAL_USER_CFG=C:\Users\REPTILE\Documents\My Games\OpenMW

if not exist "%CLIENT_DIR%" mkdir "%CLIENT_DIR%"

if not exist "%CLIENT_DIR%\openmw.cfg" (
    echo # Client 2 local overrides> "%CLIENT_DIR%\openmw.cfg"
)

(
    echo [Video]
    echo resolution x = 960
    echo resolution y = 540
    echo window mode = 2
    echo window border = true
    echo window x = %WIN_X%
    echo window y = %WIN_Y%
) > "%CLIENT_DIR%\settings.cfg"

"%OPENMW%" ^
    --config    "%REAL_USER_CFG%" ^
    --config    "%CLIENT_DIR%" ^
    --user-data "%CLIENT_DIR%" ^
    --log-dir   "%CLIENT_DIR%" ^
    --connect   127.0.0.1:25565 ^
    --mp-name   turdface

endlocal
