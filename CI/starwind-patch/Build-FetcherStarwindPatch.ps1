[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $CompatibilityBuildRoot,
    [Parameter(Mandatory = $true)][string] $OutputDirectory,
    [string] $PatchVersion = "1.0.0",
    [string] $SourceCommit = ""
)

$ErrorActionPreference = "Stop"
$sourceRoot = (Resolve-Path -LiteralPath $CompatibilityBuildRoot).Path
$sourceDataFiles = Join-Path $sourceRoot "Data Files"
$sourceOverlay = Join-Path $sourceRoot "Starwind Vanilla Compat"
foreach ($required in @(
    (Join-Path $sourceDataFiles "StarwindRemasteredV1.15.esm"),
    (Join-Path $sourceDataFiles "StarwindRemasteredPatch.esm"),
    $sourceOverlay
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required compatibility build output is missing: $required"
    }
}
$intermediateEsms = @(Get-ChildItem -LiteralPath $sourceDataFiles -File |
    Where-Object { $_.Name -match '^StarwindRemasteredV1\.15\.\d+\.esm$' })
if ($intermediateEsms.Count -gt 0) {
    Write-Host "Ignoring $($intermediateEsms.Count) numbered intermediate ESM build artifact(s)."
}

$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$stageRoot = Join-Path $outputRoot (".starwind-patch-stage-{0}" -f [Guid]::NewGuid().ToString("N"))
$payloadRoot = Join-Path $stageRoot "payload"
$payloadDataFiles = Join-Path $payloadRoot "Data Files"
$archivePath = Join-Path $outputRoot "fetcher-starwind-compat-patch-v2.zip"

try {
    New-Item -ItemType Directory -Force -Path $payloadDataFiles | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceDataFiles "StarwindRemasteredV1.15.esm") -Destination $payloadDataFiles
    Copy-Item -LiteralPath (Join-Path $sourceDataFiles "StarwindRemasteredPatch.esm") -Destination $payloadDataFiles
    Copy-Item -LiteralPath $sourceOverlay -Destination $payloadRoot -Recurse
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "Apply-Fetcher-Starwind-CompatibilityPatch.ps1") -Destination $stageRoot

    $files = @(
        Get-ChildItem -LiteralPath $payloadRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
            $relative = $_.FullName.Substring($payloadRoot.Length + 1).Replace("\", "/")
            [ordered]@{
                path = $relative
                size = [int64]$_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    )
    $manifest = [ordered]@{
        schemaVersion = 1
        patchId = "fetcher-starwind-compat"
        patchVersion = $PatchVersion
        sourceCommit = $SourceCommit
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        files = $files
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stageRoot "fetcher-starwind-compat-patch.json") -Encoding UTF8
    Copy-Item -LiteralPath (Join-Path $stageRoot "fetcher-starwind-compat-patch.json") `
        -Destination (Join-Path $stageRoot "fetcher-bardcraft-mp-patch.json")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "README.md") -Destination (Join-Path $stageRoot "README.txt")

    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }
    Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $archivePath -CompressionLevel Optimal
}
finally {
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
}

Write-Host "Created: $archivePath"
Write-Host "SHA256: $((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant())"
