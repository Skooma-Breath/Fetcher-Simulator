[CmdletBinding()]
param(
    [string] $InstallRoot = "",
    [string] $ZhiDataRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$relativeScriptPath = "scripts\ZerkishHotkeysImproved\zhi_player.lua"
$scriptPath = $null
if (-not [string]::IsNullOrWhiteSpace($ZhiDataRoot)) {
    $dataRoot = (Resolve-Path -LiteralPath $ZhiDataRoot).Path
    $candidate = Join-Path $dataRoot $relativeScriptPath
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "The selected Zerkish Hotkeys Improved data root is missing $relativeScriptPath`: $dataRoot"
    }
    $scriptPath = $candidate
}
else {
    $root = (Resolve-Path -LiteralPath $InstallRoot).Path
    $managedDataRoot = Join-Path $root `
        "Data Files\fetcher-bardcraft\UserInterface\ZerkishHotkeysImproved\ZerkishHotkeysImproved"
    $managedScriptPath = Join-Path $managedDataRoot $relativeScriptPath
    if (Test-Path -LiteralPath $managedScriptPath -PathType Leaf) {
        $scriptPath = $managedScriptPath
    }
    $dataFilesRoot = Join-Path $root "Data Files"
    if ($null -eq $scriptPath -and (Test-Path -LiteralPath $dataFilesRoot -PathType Container)) {
        $suffix = $relativeScriptPath.ToLowerInvariant()
        $candidates = @(Get-ChildItem -LiteralPath $dataFilesRoot -Recurse -Force -File `
            -Filter "zhi_player.lua" -ErrorAction SilentlyContinue | Where-Object {
                $_.FullName.ToLowerInvariant().EndsWith($suffix, [StringComparison]::OrdinalIgnoreCase)
            })
        if ($candidates.Count -eq 1) {
            $scriptPath = $candidates[0].FullName
        }
        elseif ($candidates.Count -gt 1) {
            throw "Found multiple Zerkish Hotkeys Improved installations. Remove stale duplicate data paths before updating."
        }
    }
}

if ($null -eq $scriptPath) {
    Write-Host "Zerkish Hotkeys Improved is not installed; skipping its multiplayer compatibility fix."
    return
}

$marker = "Fetcher multiplayer compatibility: suppress the first-time modal during character creation."
$source = [IO.File]::ReadAllText($scriptPath)
if ($source.Contains($marker)) {
    Write-Host "Zerkish Hotkeys Improved character-creation popup is already disabled."
    return
}

$original = "if not (ZHISaveData.onCloseQuickKeyMenuFirstTimeFlag or sDisableFirstTimeNotification) then"
$replacement = "if false and not (ZHISaveData.onCloseQuickKeyMenuFirstTimeFlag or sDisableFirstTimeNotification) then -- $marker"
$occurrences = ([regex]::Matches($source, [regex]::Escape($original))).Count
if ($occurrences -ne 1) {
    throw "Expected one compatible ZHI first-time notification guard, found $occurrences. The installed mod version may need a new compatibility fix."
}

$updated = $source.Replace($original, $replacement)
$temporaryPath = "$scriptPath.$([Guid]::NewGuid().ToString('N')).tmp"
try {
    $encoding = [Text.UTF8Encoding]::new($source.StartsWith([char]0xFEFF))
    [IO.File]::WriteAllText($temporaryPath, $updated, $encoding)
    Move-Item -LiteralPath $temporaryPath -Destination $scriptPath -Force
}
finally {
    if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }
}

Write-Host "Disabled the Zerkish Hotkeys Improved first-time character-creation popup:"
Write-Host "  $scriptPath"
