@echo off
setlocal
set "BOOTSTRAP=%TEMP%\Install-Fetcher-Tester-Tools-%RANDOM%-%RANDOM%.ps1"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $release=Invoke-RestMethod -UseBasicParsing -Headers @{'User-Agent'='Fetcher-Tester-Bootstrap'} -Uri 'https://api.github.com/repos/Skooma-Breath/Fetcher-Simulator/releases/tags/fetcher-tester-tools'; $asset=@($release.assets | Where-Object { $_.name -eq 'Install-Fetcher-Tester-Tools.ps1' }); if($asset.Count -ne 1 -or [string]$asset[0].digest -notmatch '^sha256:([0-9a-fA-F]{64})$'){throw 'The tester-tools prerelease does not contain one digest-backed bootstrap asset.'}; $expected=$Matches[1].ToLowerInvariant(); Invoke-WebRequest -UseBasicParsing -Headers @{'User-Agent'='Fetcher-Tester-Bootstrap'} -Uri $asset[0].browser_download_url -OutFile '%BOOTSTRAP%'; $actual=(Get-FileHash -LiteralPath '%BOOTSTRAP%' -Algorithm SHA256).Hash.ToLowerInvariant(); if($actual -ne $expected){throw ('Bootstrap checksum mismatch. Expected {0}, got {1}.' -f $expected,$actual)}; $arguments=@{}; if(Test-Path -LiteralPath (Join-Path '%~dp0' 'openmw.exe')){$arguments.InstallRoot='%~dp0'}; & '%BOOTSTRAP%' @arguments"
set "RESULT=%ERRORLEVEL%"
del /q "%BOOTSTRAP%" >nul 2>nul
if not "%RESULT%"=="0" (
  echo.
  echo Fetcher test-channel setup failed with exit code %RESULT%.
  pause
)
exit /b %RESULT%

