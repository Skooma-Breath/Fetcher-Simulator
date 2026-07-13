$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfgPath = Join-Path $root "openmw.cfg"
$umoModListName = "fetcher-bardcraft"
$umoModListPath = Join-Path $root "fetcher-bardcraft-umo.json"
$patchCatalogPath = Join-Path $root "fetcher-client-patches.json"

$baseContent = @(
    "Morrowind.esm",
    "Tribunal.esm",
    "Bloodmoon.esm"
)
$fetcherMapContent = @(
    "surf_mesa_mw.omwaddon",
    "surf_utopia_mw.omwaddon",
    "surf_kitsune.omwaddon",
    "surf_kitsune.omwscripts",
    "surf_kitsune2.omwaddon",
    "mp_phase7_test.omwscripts"
)
$earlyContentOrder = @(
    "StarwindRemasteredV1.15.esm",
    "StarwindRemasteredPatch.esm",
    "StarwindVanillaCompat.omwscripts",
    "Tamriel_Data.esm",
    "Tamriel_Data.omwscripts",
    "Tamriel Data Races Playable 25.05.ESP",
    "OAAB_Data.esm",
    "TR_Mainland.esm",
    "TR_Factions.esp",
    "tamrielrebuilt.omwscripts",
    "Cyr_Main.esm"
)

function Add-UniqueContent {
    param(
        [Parameter(Mandatory = $true)] $Target,
        [Parameter(Mandatory = $true)][string] $Value
    )
    if (-not $Target.Contains($Value)) {
        $Target.Add($Value)
    }
}

function Get-OptionalPropertyValues {
    param(
        [Parameter(Mandatory = $true)] $Object,
        [Parameter(Mandatory = $true)][string] $Name
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property -and $null -ne $property.Value) {
        foreach ($value in @($property.Value)) {
            $value
        }
    }
}

$umoMods = @()
if (Test-Path -LiteralPath $umoModListPath -PathType Leaf) {
    $umoMods = @(Get-Content -Raw -LiteralPath $umoModListPath | ConvertFrom-Json)
}
$patches = @()
if (Test-Path -LiteralPath $patchCatalogPath -PathType Leaf) {
    $patchCatalog = Get-Content -Raw -LiteralPath $patchCatalogPath | ConvertFrom-Json
    if ([int]$patchCatalog.schemaVersion -ne 1) {
        throw "Unsupported Fetcher client patch catalog schema: $($patchCatalog.schemaVersion)"
    }
    $patches = @($patchCatalog.patches)
}

$availableUmoContent = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::OrdinalIgnoreCase)
foreach ($mod in $umoMods) {
    foreach ($plugin in @($mod.plugins)) {
        [void]$availableUmoContent.Add([string]$plugin)
    }
}
$availablePatchContent = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::OrdinalIgnoreCase)
foreach ($patch in $patches) {
    $allPatchPathsExist = $true
    foreach ($dataPath in @(Get-OptionalPropertyValues -Object $patch -Name "dataPaths")) {
        if (-not (Test-Path -LiteralPath (Join-Path $root ([string]$dataPath)))) {
            $allPatchPathsExist = $false
            break
        }
    }
    if ($allPatchPathsExist) {
        foreach ($plugin in @(Get-OptionalPropertyValues -Object $patch -Name "plugins")) {
            if (-not [string]::IsNullOrWhiteSpace([string]$plugin)) {
                [void]$availablePatchContent.Add([string]$plugin)
            }
        }
    }
}

$requiredContentList = New-Object System.Collections.Generic.List[string]
foreach ($content in $baseContent) {
    Add-UniqueContent -Target $requiredContentList -Value $content
}
foreach ($content in $earlyContentOrder) {
    if ($availableUmoContent.Contains($content) -or $availablePatchContent.Contains($content)) {
        Add-UniqueContent -Target $requiredContentList -Value $content
    }
}
foreach ($content in $fetcherMapContent) {
    Add-UniqueContent -Target $requiredContentList -Value $content
}
foreach ($mod in $umoMods) {
    foreach ($plugin in @($mod.plugins)) {
        if ($earlyContentOrder -notcontains [string]$plugin) {
            Add-UniqueContent -Target $requiredContentList -Value ([string]$plugin)
        }
    }
}
foreach ($patch in $patches) {
    foreach ($plugin in @(Get-OptionalPropertyValues -Object $patch -Name "plugins")) {
        if (-not [string]::IsNullOrWhiteSpace([string]$plugin) -and
            $availablePatchContent.Contains([string]$plugin) -and $earlyContentOrder -notcontains [string]$plugin) {
            Add-UniqueContent -Target $requiredContentList -Value ([string]$plugin)
        }
    }
}
$requiredContent = @($requiredContentList)

if (-not (Test-Path -LiteralPath $cfgPath)) {
    throw "Could not find openmw.cfg next to this script: $cfgPath"
}

$existingLines = @(Get-Content -LiteralPath $cfgPath)
$backupPath = Join-Path $root ("openmw.cfg.before-fetcher-public-test-{0}.bak" -f (Get-Date -Format "yyyyMMdd-HHmmss"))

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

function Get-PatchDataPathEntries {
    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($patch in $patches) {
        foreach ($dataPath in @(Get-OptionalPropertyValues -Object $patch -Name "dataPaths")) {
            if ([string]::IsNullOrWhiteSpace([string]$dataPath)) {
                continue
            }
            $relativePath = Join-ConfigPath @([string]$dataPath)
            $entries.Add([pscustomobject]@{
                ModName = [string]$patch.name
                ConfigPath = $relativePath
                AbsolutePath = Join-Path $root ([string]$dataPath)
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
$patchDataEntries = @(Get-PatchDataPathEntries)
$existingManagedDataEntries = @(@($umoDataEntries) + @($patchDataEntries) |
    Where-Object { Test-Path -LiteralPath $_.AbsolutePath })

$newLines = New-Object System.Collections.Generic.List[string]
$newLines.AddRange([string[]]$filteredLines)
if ($existingManagedDataEntries.Count -gt 0) {
    $newLines.Add("")
    $newLines.Add($dataBeginMarker)
    foreach ($entry in $existingManagedDataEntries) {
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

$stagedCfgPath = Join-Path $root (".openmw.cfg.fetcher-{0}.tmp" -f [Guid]::NewGuid().ToString("N"))
try {
    Set-Content -LiteralPath $stagedCfgPath -Value $newLines -Encoding ASCII
    $configWritten = $false
    $lastWriteError = $null
    for ($attempt = 1; $attempt -le 8; ++$attempt) {
        try {
            [System.IO.File]::Replace($stagedCfgPath, $cfgPath, $backupPath, $true)
            $configWritten = $true
            break
        }
        catch [System.IO.IOException] {
            $lastWriteError = $_.Exception
            Start-Sleep -Milliseconds 250
        }
    }
    if (-not $configWritten) {
        throw "Could not update $cfgPath after repeated attempts. Close OpenMW, the launcher, and any editor using openmw.cfg, then run this script again. Details: $($lastWriteError.Message)"
    }
}
finally {
    if (Test-Path -LiteralPath $stagedCfgPath) {
        Remove-Item -LiteralPath $stagedCfgPath -Force
    }
}

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
if ($existingManagedDataEntries.Count -gt 0) {
    Write-Host "Fetcher-managed data paths added:"
    foreach ($entry in $existingManagedDataEntries) {
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
