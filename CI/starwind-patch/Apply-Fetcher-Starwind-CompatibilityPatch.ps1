[CmdletBinding()]
param(
    [string] $InstallRoot = "",
    [Parameter(Mandatory = $true)][string] $StarwindDataRoot
)

$ErrorActionPreference = "Stop"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string] $Path)
    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Assert-PathInside {
    param(
        [Parameter(Mandatory = $true)][string] $Parent,
        [Parameter(Mandatory = $true)][string] $Child,
        [Parameter(Mandatory = $true)][string] $Description
    )
    $parentPath = (Get-FullPath $Parent) + [IO.Path]::DirectorySeparatorChar
    $childPath = Get-FullPath $Child
    if (-not $childPath.StartsWith($parentPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description is outside the expected root: $childPath"
    }
}

$sourcePath = Get-FullPath $StarwindDataRoot
if (-not (Test-Path -LiteralPath $sourcePath -PathType Container)) {
    throw "Starwind data root does not exist: $sourcePath"
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $candidate = $sourcePath
    while ($true) {
        if ((Test-Path -LiteralPath (Join-Path $candidate "openmw.cfg") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $candidate "openmw.exe") -PathType Leaf)) {
            $InstallRoot = $candidate
            break
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $candidate) {
            throw "Could not infer the Fetcher installation root from: $sourcePath"
        }
        $candidate = $parent
    }
}
$installPath = Get-FullPath $InstallRoot
if (-not (Test-Path -LiteralPath $installPath -PathType Container)) {
    throw "Install root does not exist: $installPath"
}
Assert-PathInside -Parent $installPath -Child $sourcePath -Description "Starwind data root"

foreach ($requiredPath in @("StarwindRemasteredV1.15.esm", "StarwindRemasteredPatch.esm", "Meshes")) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourcePath $requiredPath))) {
        throw "The selected Starwind data root is missing $requiredPath`: $sourcePath"
    }
}

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $packageRoot "fetcher-starwind-compat-patch.json"
$payloadRoot = Join-Path $packageRoot "payload"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Patch manifest is missing: $manifestPath"
}
if (-not (Test-Path -LiteralPath $payloadRoot -PathType Container)) {
    throw "Patch payload is missing: $payloadRoot"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ([int]$manifest.schemaVersion -ne 1 -or [string]$manifest.patchId -ne "fetcher-starwind-compat") {
    throw "The Starwind compatibility patch manifest is unsupported."
}
$manifestFiles = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::OrdinalIgnoreCase)
foreach ($file in @($manifest.files)) {
    $relativePath = ([string]$file.path).Replace("/", [IO.Path]::DirectorySeparatorChar)
    if ([string]::IsNullOrWhiteSpace($relativePath) -or [IO.Path]::IsPathRooted($relativePath) -or
        @($relativePath.Split([IO.Path]::DirectorySeparatorChar)) -contains "..") {
        throw "Unsafe payload path in manifest: $($file.path)"
    }
    $payloadPath = Get-FullPath (Join-Path $payloadRoot $relativePath)
    if (-not $manifestFiles.Add(([string]$file.path).Replace("\", "/"))) {
        throw "Duplicate payload path in manifest: $($file.path)"
    }
    Assert-PathInside -Parent $payloadRoot -Child $payloadPath -Description "Payload file"
    if (-not (Test-Path -LiteralPath $payloadPath -PathType Leaf)) {
        throw "Payload file is missing: $($file.path)"
    }
    $payloadFile = Get-Item -LiteralPath $payloadPath
    if ([int64]$payloadFile.Length -ne [int64]$file.size -or (Get-Sha256 $payloadPath) -ne ([string]$file.sha256).ToLowerInvariant()) {
        throw "Payload verification failed: $($file.path)"
    }
}
$actualPayloadFiles = @(Get-ChildItem -LiteralPath $payloadRoot -Recurse -File)
if ($actualPayloadFiles.Count -ne $manifestFiles.Count) {
    throw "The payload contains files that are absent from its manifest."
}
foreach ($payloadFile in $actualPayloadFiles) {
    $relativePath = $payloadFile.FullName.Substring($payloadRoot.Length + 1).Replace("\", "/")
    if (-not $manifestFiles.Contains($relativePath)) {
        throw "Unmanifested payload file: $relativePath"
    }
}

$dataFilesRoot = Join-Path $installPath "Data Files"
New-Item -ItemType Directory -Force -Path $dataFilesRoot | Out-Null
$targetPath = Join-Path $dataFilesRoot "fetcher-starwind-compat"
Assert-PathInside -Parent $dataFilesRoot -Child $targetPath -Description "Managed patch directory"
$transactionId = [Guid]::NewGuid().ToString("N")
$stagePath = Join-Path $dataFilesRoot (".fetcher-starwind-compat-stage-{0}" -f $transactionId)
$backupPath = Join-Path $dataFilesRoot (".fetcher-starwind-compat-backup-{0}" -f $transactionId)

try {
    New-Item -ItemType Directory -Path $stagePath | Out-Null
    Copy-Item -LiteralPath (Join-Path $payloadRoot "Data Files") -Destination $stagePath -Recurse
    Copy-Item -LiteralPath (Join-Path $payloadRoot "Starwind Vanilla Compat") -Destination $stagePath -Recurse
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $stagePath "fetcher-starwind-compat-patch.json")

    if (Test-Path -LiteralPath $targetPath) {
        Move-Item -LiteralPath $targetPath -Destination $backupPath
    }
    try {
        Move-Item -LiteralPath $stagePath -Destination $targetPath
    }
    catch {
        if (Test-Path -LiteralPath $backupPath) {
            Move-Item -LiteralPath $backupPath -Destination $targetPath
        }
        throw
    }
    if (Test-Path -LiteralPath $backupPath) {
        Remove-Item -LiteralPath $backupPath -Recurse -Force
    }
}
finally {
    if (Test-Path -LiteralPath $stagePath) {
        Remove-Item -LiteralPath $stagePath -Recurse -Force
    }
}

Write-Host "Installed Starwind compatibility patch $($manifest.patchVersion):"
Write-Host "  $targetPath"

$starwindTexturesPath = Join-Path $sourcePath "Textures"
$uiQuarantinePath = Join-Path $installPath "_fetcher_update\quarantine\starwind-ui\Textures"
Assert-PathInside -Parent $installPath -Child $uiQuarantinePath -Description "Starwind UI quarantine directory"
$quarantinedUiFiles = 0
if (Test-Path -LiteralPath $starwindTexturesPath -PathType Container) {
    New-Item -ItemType Directory -Force -Path $uiQuarantinePath | Out-Null
    $datapadTexturePath = Join-Path $starwindTexturesPath "JMenuScreen.dds"
    $quarantinedDatapadTexturePath = Join-Path $uiQuarantinePath "JMenuScreen.dds"
    if (Test-Path -LiteralPath $quarantinedDatapadTexturePath -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $datapadTexturePath -PathType Leaf)) {
            Move-Item -LiteralPath $quarantinedDatapadTexturePath -Destination $datapadTexturePath -Force
            Write-Host "Restored Starwind datapad screen texture: JMenuScreen.dds"
        }
        else {
            Remove-Item -LiteralPath $quarantinedDatapadTexturePath -Force
        }
    }
    foreach ($uiTexture in Get-ChildItem -LiteralPath $starwindTexturesPath -File | Where-Object {
        $_.Name -match '^(?i:menu_|tx_menu|scroll|cursor).*\.dds$'
    }) {
        Move-Item -LiteralPath $uiTexture.FullName -Destination (Join-Path $uiQuarantinePath $uiTexture.Name) -Force
        ++$quarantinedUiFiles
    }
}
Write-Host "Quarantined $quarantinedUiFiles Starwind root UI texture override(s)."

$configScript = Join-Path $installPath "Apply-Fetcher-Public-Test-Config.ps1"
if (-not (Test-Path -LiteralPath $configScript -PathType Leaf)) {
    throw "Fetcher public test configuration script is missing: $configScript"
}
Write-Host "Regenerating openmw.cfg after the Starwind compatibility patch..."
& $configScript
if (-not $?) {
    throw "Fetcher public test configuration regeneration failed."
}
