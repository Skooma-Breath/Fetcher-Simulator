param(
    [string] $MpClientsDir = "C:\serena_workspaces_directory\mp-clients",
    [string] $ClientConfig = "client1\openmw.cfg",
    [string] $OutputPath = "C:\tmp\openmw-client-mods.zip"
)

$ErrorActionPreference = "Stop"

function Copy-FilteredDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Source,
        [Parameter(Mandatory = $true)]
        [string] $Destination
    )

    $excludedExtensions = @(".bak", ".log", ".dmp", ".db")
    $excludedNamePatterns = @("*.bak", "*.bak.*", "*.pre_*", "*.old", "*.tmp")

    $sourceFullPath = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\', '/')
    Get-ChildItem -LiteralPath $sourceFullPath -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($sourceFullPath.Length).TrimStart('\', '/')
        $excluded = $excludedExtensions -contains $_.Extension.ToLowerInvariant()
        foreach ($pattern in $excludedNamePatterns) {
            if ($_.Name -like $pattern) {
                $excluded = $true
                break
            }
        }
        if ($excluded) {
            return
        }

        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}

$mpRoot = (Resolve-Path -LiteralPath $MpClientsDir).Path
$cfgPath = Join-Path $mpRoot $ClientConfig
if (-not (Test-Path -LiteralPath $cfgPath)) {
    throw "Client config was not found: $cfgPath"
}

$cfgLines = Get-Content -LiteralPath $cfgPath
$dataDirs = @()
$content = @()
$fallbackArchives = @()
foreach ($line in $cfgLines) {
    if ($line -match '^\s*data=(.+?)\s*$') {
        $value = $matches[1].Trim().Trim('"')
        if ($value -like "$mpRoot*") {
            $dataDirs += $value
        }
    }
    elseif ($line -match '^\s*content=(.+?)\s*$') {
        $content += $matches[1].Trim()
    }
    elseif ($line -match '^\s*fallback-archive=(.+?)\s*$') {
        $fallbackArchives += $matches[1].Trim()
    }
}

if (-not $dataDirs) {
    throw "No mp-clients data directories were found in $cfgPath."
}

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("openmw-client-mods-" + [guid]::NewGuid().ToString("N"))
$dataFilesStage = Join-Path $stage "Data Files"
New-Item -ItemType Directory -Force $dataFilesStage | Out-Null

foreach ($dataDir in $dataDirs) {
    if (-not (Test-Path -LiteralPath $dataDir)) {
        throw "Configured data directory does not exist: $dataDir"
    }
    Copy-FilteredDirectory -Source $dataDir -Destination $dataFilesStage
}

$manifest = [ordered]@{
    fallbackArchives = $fallbackArchives
    dataDirs = @("./Data Files")
    content = $content
    userData = "./userdata"
    sourceConfig = $cfgPath
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage "openmw-client-package.json") -Encoding ASCII

$outputFullPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
New-Item -ItemType Directory -Force (Split-Path -Parent $outputFullPath) | Out-Null
if (Test-Path -LiteralPath $outputFullPath) {
    Remove-Item -LiteralPath $outputFullPath -Force
}
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $outputFullPath -CompressionLevel Optimal -Force
Remove-Item -LiteralPath $stage -Recurse -Force

$bytes = (Get-Item -LiteralPath $outputFullPath).Length
Write-Output "Created $outputFullPath ($([math]::Round($bytes / 1MB, 2)) MiB)."
