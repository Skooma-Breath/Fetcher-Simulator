[CmdletBinding()]
param(
    [string] $InstallRoot = "",
    [string] $ServerTarget = "",
    [string] $Account = "",
    [string] $Character = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$root = (Resolve-Path -LiteralPath $InstallRoot).Path.TrimEnd("\", "/")
$openmw = Join-Path $root "openmw.exe"
if (-not (Test-Path -LiteralPath $openmw -PathType Leaf)) {
    throw "openmw.exe was not found beside this launcher: $root"
}

if ([string]::IsNullOrWhiteSpace($ServerTarget)) {
    $ServerTarget = $env:SERVER_TARGET
}
if ([string]::IsNullOrWhiteSpace($ServerTarget)) {
    $ServerTarget = "164.152.19.250:25564"
}
if ([string]::IsNullOrWhiteSpace($Account)) {
    $Account = $env:MP_ACCOUNT
}
if ([string]::IsNullOrWhiteSpace($Account)) {
    $Account = Read-Host "Account name"
}
if ([string]::IsNullOrWhiteSpace($Character)) {
    $Character = $env:MP_CHARACTER
}
if ([string]::IsNullOrWhiteSpace($Character)) {
    $Character = Read-Host "Character name"
}
if ([string]::IsNullOrWhiteSpace($Account) -or [string]::IsNullOrWhiteSpace($Character)) {
    throw "Account and character names are required."
}

$profileRoot = Join-Path $root "profiles"
$arguments = @(
    "--connect", $ServerTarget,
    "--mp-account", $Account,
    "--mp-character", $Character,
    "--mp-auto-enter=1",
    "--mp-profile-root", $profileRoot
)

Write-Host "Launching Fetcher Simulator"
Write-Host "  Server:    $ServerTarget"
Write-Host "  Account:   $Account"
Write-Host "  Character: $Character"
Write-Host "  Profiles:  $profileRoot"

Push-Location $root
try {
    & $openmw @arguments
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
