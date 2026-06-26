$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfgPath = Join-Path $root "openmw.cfg"
$umoModListName = "fetcher-bardcraft"
$umoModListPath = Join-Path $root "fetcher-bardcraft-umo.json"

$requiredContent = @(
    "Morrowind.esm",
    "Tribunal.esm",
    "Bloodmoon.esm",
    "Tamriel_Data.esm",
    "surf_mesa_mw.omwaddon",
    "surf_utopia_mw.omwaddon",
    "surf_kitsune.omwaddon",
    "surf_deathstar.omwaddon",
    "surf_deathstar.omwscripts",
    "surf_kitsune.omwscripts",
    "surf_kitsune2.omwaddon",
    "mp_phase7_test.omwscripts",
    "StatsWindow.ESP",
    "StatsWindow.omwscripts",
    "SkillFramework.omwscripts",
    "Bardcraft.ESP",
    "Bardcraft.omwscripts",
    "Tamriel_Data.omwscripts"
)

if (-not (Test-Path -LiteralPath $cfgPath)) {
    throw "Could not find openmw.cfg next to this script: $cfgPath"
}

$existingLines = @(Get-Content -LiteralPath $cfgPath)
$backupPath = Join-Path $root ("openmw.cfg.before-fetcher-public-test-{0}.bak" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
Copy-Item -LiteralPath $cfgPath -Destination $backupPath -Force

$dataBeginMarker = "# BEGIN Fetcher Simulator UMO data paths"
$dataEndMarker = "# END Fetcher Simulator UMO data paths"
$beginMarker = "# BEGIN Fetcher Simulator public test load order"
$endMarker = "# END Fetcher Simulator public test load order"
$filteredLines = New-Object System.Collections.Generic.List[string]
$insideFetcherBlock = $false
$insideFetcherDataBlock = $false

foreach ($line in $existingLines) {
    $trimmed = $line.Trim()
    if ($trimmed -eq $dataBeginMarker) {
        $insideFetcherDataBlock = $true
        continue
    }
    if ($trimmed -eq $dataEndMarker) {
        $insideFetcherDataBlock = $false
        continue
    }
    if ($insideFetcherDataBlock) {
        continue
    }
    if ($trimmed -eq $beginMarker) {
        $insideFetcherBlock = $true
        continue
    }
    if ($trimmed -eq $endMarker) {
        $insideFetcherBlock = $false
        continue
    }
    if ($insideFetcherBlock) {
        continue
    }
    if ($trimmed -match "^content\s*=") {
        continue
    }
    $filteredLines.Add($line)
}

while ($filteredLines.Count -gt 0 -and [string]::IsNullOrWhiteSpace($filteredLines[$filteredLines.Count - 1])) {
    $filteredLines.RemoveAt($filteredLines.Count - 1)
}

function Join-ConfigPath {
    param([string[]] $Parts)

    $clean = New-Object System.Collections.Generic.List[string]
    foreach ($part in $Parts) {
        if ([string]::IsNullOrWhiteSpace($part)) {
            continue
        }
        $clean.Add(($part.Trim() -replace "\\", "/").Trim("/"))
    }
    return "./" + ($clean -join "/")
}

function Get-UmoDataPathEntries {
    $entries = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $umoModListPath)) {
        return
    }

    $mods = Get-Content -Raw -LiteralPath $umoModListPath | ConvertFrom-Json
    foreach ($mod in $mods) {
        foreach ($dataPath in @($mod.data_paths)) {
            if ([string]::IsNullOrWhiteSpace($dataPath)) {
                continue
            }

            $relativePath = Join-ConfigPath @(
                "Data Files",
                $umoModListName,
                [string]$mod.category,
                [string]$dataPath
            )
            $absolutePath = Join-Path (Join-Path (Join-Path (Join-Path $root "Data Files") $umoModListName) ([string]$mod.category)) ([string]$dataPath)
            $entries.Add([pscustomobject]@{
                ModName = [string]$mod.name
                ConfigPath = $relativePath
                AbsolutePath = $absolutePath
            })
        }
    }
    foreach ($entry in $entries) {
        $entry
    }
}

$existingDataLines = @{}
foreach ($line in $filteredLines) {
    $trimmed = $line.Trim()
    if ($trimmed -match "^data\s*=") {
        $existingDataLines[$trimmed.ToLowerInvariant()] = $true
    }
}

$umoDataEntries = @(Get-UmoDataPathEntries)
$existingUmoDataEntries = @($umoDataEntries | Where-Object { Test-Path -LiteralPath $_.AbsolutePath })

$newLines = New-Object System.Collections.Generic.List[string]
$newLines.AddRange([string[]]$filteredLines)
if ($existingUmoDataEntries.Count -gt 0) {
    $newLines.Add("")
    $newLines.Add($dataBeginMarker)
    foreach ($entry in $existingUmoDataEntries) {
        $dataLine = "data=$($entry.ConfigPath)"
        if (-not $existingDataLines.ContainsKey($dataLine.ToLowerInvariant())) {
            $newLines.Add($dataLine)
        }
    }
    $newLines.Add($dataEndMarker)
}
$newLines.Add("")
$newLines.Add($beginMarker)
foreach ($content in $requiredContent) {
    $newLines.Add("content=$content")
}
$newLines.Add($endMarker)
$newLines.Add("")

Set-Content -LiteralPath $cfgPath -Value $newLines -Encoding ASCII

function Convert-ToDataPath {
    param([string] $Value)

    $path = $Value.Trim()
    if (($path.StartsWith('"') -and $path.EndsWith('"')) -or ($path.StartsWith("'") -and $path.EndsWith("'"))) {
        $path = $path.Substring(1, $path.Length - 2)
    }
    $path = [Environment]::ExpandEnvironmentVariables($path)
    if ([System.IO.Path]::IsPathRooted($path)) {
        return $path
    }
    return (Join-Path $root $path)
}

$dataDirs = New-Object System.Collections.Generic.List[string]
foreach ($line in $newLines) {
    if ($line -match "^\s*data\s*=\s*(.+?)\s*$") {
        $dataPath = Convert-ToDataPath $Matches[1]
        if (-not [string]::IsNullOrWhiteSpace($dataPath)) {
            $dataDirs.Add($dataPath)
        }
    }
}

$missing = New-Object System.Collections.Generic.List[string]
foreach ($content in $requiredContent) {
    $found = $false
    foreach ($dir in $dataDirs) {
        if (Test-Path -LiteralPath (Join-Path $dir $content)) {
            $found = $true
            break
        }
    }
    if (-not $found) {
        $missing.Add($content)
    }
}

Write-Host "Updated: $cfgPath"
Write-Host "Backup:  $backupPath"
Write-Host ""
if ($existingUmoDataEntries.Count -gt 0) {
    Write-Host "UMO data paths added from ${umoModListName}:"
    foreach ($entry in $existingUmoDataEntries) {
        Write-Host "  data=$($entry.ConfigPath)"
    }
    Write-Host ""
}
Write-Host "Public test content lines written:"
foreach ($content in $requiredContent) {
    Write-Host "  content=$content"
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Warning: these files were not found in the data= folders currently listed in openmw.cfg:"
    foreach ($content in $missing) {
        Write-Host "  $content"
    }
    Write-Host ""
    Write-Host "Install the missing mods or add the correct data= folders to openmw.cfg, then run this BAT again."
} else {
    Write-Host ""
    Write-Host "All required content files were found in configured data= folders."
}
