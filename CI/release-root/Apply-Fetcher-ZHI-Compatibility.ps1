[CmdletBinding()]
param(
    [string] $InstallRoot = "",
    [string] $ZhiDataRoot = "",
    [string] $HookshotDataRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# This filename is retained for compatibility with older tester-tool updaters.
# The script now applies small, idempotent compatibility fixes to multiple UMO mods.

function Test-DataRoot {
    param(
        [Parameter(Mandatory = $true)][string] $DataRoot,
        [Parameter(Mandatory = $true)][string[]] $RequiredFiles
    )

    foreach ($relativePath in $RequiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $DataRoot $relativePath) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Resolve-ModDataRoot {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $DisplayName,
        [Parameter(Mandatory = $true)][string] $ManagedRelativeRoot,
        [Parameter(Mandatory = $true)][string] $PrimaryRelativeFile,
        [Parameter(Mandatory = $true)][string[]] $RequiredFiles,
        [string] $ExplicitDataRoot = ""
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitDataRoot)) {
        $dataRoot = (Resolve-Path -LiteralPath $ExplicitDataRoot).Path
        if (-not (Test-DataRoot -DataRoot $dataRoot -RequiredFiles $RequiredFiles)) {
            throw "The selected $DisplayName data root is missing one or more required files: $dataRoot"
        }
        return $dataRoot
    }

    $managedDataRoot = Join-Path $Root $ManagedRelativeRoot
    if (Test-DataRoot -DataRoot $managedDataRoot -RequiredFiles $RequiredFiles) {
        return $managedDataRoot
    }

    $dataFilesRoot = Join-Path $Root "Data Files"
    if (-not (Test-Path -LiteralPath $dataFilesRoot -PathType Container)) {
        return $null
    }

    $primaryFileName = Split-Path -Leaf $PrimaryRelativeFile
    $normalizedSuffix = $PrimaryRelativeFile.Replace("/", "\").ToLowerInvariant()
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($primaryFile in Get-ChildItem -LiteralPath $dataFilesRoot -Recurse -Force -File `
        -Filter $primaryFileName -ErrorAction SilentlyContinue) {
        $normalizedPath = $primaryFile.FullName.ToLowerInvariant()
        if (-not $normalizedPath.EndsWith($normalizedSuffix, [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        $candidateRoot = $primaryFile.FullName.Substring(0, $primaryFile.FullName.Length - $PrimaryRelativeFile.Length).TrimEnd("\", "/")
        if (Test-DataRoot -DataRoot $candidateRoot -RequiredFiles $RequiredFiles) {
            $candidates.Add($candidateRoot)
        }
    }

    $uniqueCandidates = @($candidates | Sort-Object -Unique)
    if ($uniqueCandidates.Count -eq 1) {
        return $uniqueCandidates[0]
    }
    if ($uniqueCandidates.Count -gt 1) {
        throw "Found multiple $DisplayName installations. Remove stale duplicate data paths before updating."
    }
    return $null
}

function Write-TextFileAtomically {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Text
    )

    $originalBytes = [IO.File]::ReadAllBytes($Path)
    $hasUtf8Bom = $originalBytes.Length -ge 3 -and
        $originalBytes[0] -eq 0xEF -and $originalBytes[1] -eq 0xBB -and $originalBytes[2] -eq 0xBF
    $encoding = [Text.UTF8Encoding]::new($hasUtf8Bom)
    $temporaryPath = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporaryPath, $Text, $encoding)
        Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Apply-ZhiCompatibilityFixes {
    param([Parameter(Mandatory = $true)][string] $DataRoot)

    $playerScriptPath = Join-Path $DataRoot "scripts\ZerkishHotkeysImproved\zhi_player.lua"
    $hotbarScriptPath = Join-Path $DataRoot "scripts\ZerkishHotkeysImproved\zhi_hotbarhud.lua"

    $legacyMarker = "Fetcher multiplayer compatibility: suppress the first-time modal during character creation."
    $legacyOriginal = "if not (ZHISaveData.onCloseQuickKeyMenuFirstTimeFlag or sDisableFirstTimeNotification) then"
    $legacyReplacement = "if false and not (ZHISaveData.onCloseQuickKeyMenuFirstTimeFlag or sDisableFirstTimeNotification) then -- $legacyMarker"
    $playerSource = [IO.File]::ReadAllText($playerScriptPath)
    $playerUpdated = $playerSource
    if ($playerSource.Contains($legacyMarker)) {
        $occurrences = ([regex]::Matches($playerSource, [regex]::Escape($legacyReplacement))).Count
        if ($occurrences -ne 1) {
            throw "Expected one legacy Fetcher ZHI edit, found $occurrences. Refusing to modify an unexpected script."
        }
        $playerUpdated = $playerSource.Replace($legacyReplacement, $legacyOriginal)
    }
    if ($playerUpdated -ne $playerSource) {
        Write-TextFileAtomically -Path $playerScriptPath -Text $playerUpdated
        Write-Host "Removed the legacy Fetcher edit from Zerkish Hotkeys Improved:"
        Write-Host "  $playerScriptPath"
    }

    $hotbarSource = [IO.File]::ReadAllText($hotbarScriptPath)
    $invalidAutoSizePattern = '(?m)(?<prefix>^[ \t]*name[ \t]*=[ \t]*["'']icon["''],\r?\n[ \t]*props[ \t]*=[ \t]*\{\r?\n)[ \t]*autoSize[ \t]*=[ \t]*true,\r?\n(?=[ \t]*inheritAlpha[ \t]*=[ \t]*false,)'
    $invalidAutoSizeRegex = [regex]::new($invalidAutoSizePattern, [Text.RegularExpressions.RegexOptions]::Multiline)
    $hotbarMatches = $invalidAutoSizeRegex.Matches($hotbarSource)
    if ($hotbarMatches.Count -gt 1) {
        throw "Expected at most one invalid ZHI hotbar icon autoSize property, found $($hotbarMatches.Count)."
    }
    if ($hotbarMatches.Count -eq 1) {
        $hotbarUpdated = $invalidAutoSizeRegex.Replace($hotbarSource, '${prefix}', 1)
        Write-TextFileAtomically -Path $hotbarScriptPath -Text $hotbarUpdated
        Write-Host "Removed the invalid ZHI hotbar icon autoSize property:"
        Write-Host "  $hotbarScriptPath"
    }
    else {
        Write-Host "Zerkish Hotkeys Improved hotbar layout is already compatible."
    }
}

function Apply-HookshotCompatibilityFixes {
    param([Parameter(Mandatory = $true)][string] $DataRoot)

    $playerScriptPath = Join-Path $DataRoot "scripts\OpenMWHookshot\player.lua"
    $source = [IO.File]::ReadAllText($playerScriptPath)
    $invalidTopLevelSizePattern = '(?m)(?<prefix>^[ \t]*type[ \t]*=[ \t]*ui\.TYPE\.Image,\r?\n)[ \t]*size[ \t]*=[ \t]*util\.vector2\([ \t]*BASE_RETICLE_SIZE[ \t]*,[ \t]*BASE_RETICLE_SIZE[ \t]*\),[^\r\n]*\r?\n(?=[ \t]*props[ \t]*=[ \t]*\{)'
    $invalidTopLevelSizeRegex = [regex]::new($invalidTopLevelSizePattern, [Text.RegularExpressions.RegexOptions]::Multiline)
    $matches = $invalidTopLevelSizeRegex.Matches($source)
    if ($matches.Count -gt 1) {
        throw "Expected at most one invalid OpenMWHookshot top-level size property, found $($matches.Count)."
    }
    if ($matches.Count -eq 1) {
        $updated = $invalidTopLevelSizeRegex.Replace($source, '${prefix}', 1)
        Write-TextFileAtomically -Path $playerScriptPath -Text $updated
        Write-Host "Removed the invalid OpenMWHookshot top-level reticle size property:"
        Write-Host "  $playerScriptPath"
    }
    else {
        Write-Host "OpenMWHookshot reticle layout is already compatible."
    }
}

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$root = (Resolve-Path -LiteralPath $InstallRoot).Path

$zhiRequiredFiles = @(
    "scripts\ZerkishHotkeysImproved\zhi_player.lua",
    "scripts\ZerkishHotkeysImproved\zhi_hotbarhud.lua"
)
$zhiRoot = Resolve-ModDataRoot -Root $root -DisplayName "Zerkish Hotkeys Improved" `
    -ManagedRelativeRoot "Data Files\fetcher-bardcraft\UserInterface\ZerkishHotkeysImproved\ZerkishHotkeysImproved" `
    -PrimaryRelativeFile $zhiRequiredFiles[0] -RequiredFiles $zhiRequiredFiles -ExplicitDataRoot $ZhiDataRoot
if ($null -eq $zhiRoot) {
    Write-Host "Zerkish Hotkeys Improved is not installed; skipping its compatibility fixes."
}
else {
    Apply-ZhiCompatibilityFixes -DataRoot $zhiRoot
}

$hookshotRequiredFiles = @("scripts\OpenMWHookshot\player.lua")
$hookshotRoot = Resolve-ModDataRoot -Root $root -DisplayName "OpenMWHookshot" `
    -ManagedRelativeRoot "Data Files\fetcher-bardcraft\Gameplay\OpenMWHookshot\OpenMWHookshot" `
    -PrimaryRelativeFile $hookshotRequiredFiles[0] -RequiredFiles $hookshotRequiredFiles `
    -ExplicitDataRoot $HookshotDataRoot
if ($null -eq $hookshotRoot) {
    Write-Host "OpenMWHookshot is not installed; skipping its compatibility fix."
}
else {
    Apply-HookshotCompatibilityFixes -DataRoot $hookshotRoot
}
