[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $OutputDirectory,
    [string] $PatchVersion = "1.0.0",
    [string] $SourceCommit = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($SourceCommit -and $SourceCommit -notmatch "^[0-9a-fA-F]{40}$") {
    throw "SourceCommit must be empty or a full 40-character Git commit hash."
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$applierPath = Join-Path $repoRoot "CI\release-root\Apply-Fetcher-ZHI-Compatibility.ps1"
if (-not (Test-Path -LiteralPath $applierPath -PathType Leaf)) {
    throw "ZHI compatibility applier was not found: $applierPath"
}

$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$stageRoot = Join-Path $outputRoot (".zhi-patch-stage-{0}" -f [Guid]::NewGuid().ToString("N"))
$archivePath = Join-Path $outputRoot "fetcher-zhi-compat-patch-v1.zip"

try {
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
    Copy-Item -LiteralPath $applierPath -Destination $stageRoot
    $manifest = [ordered]@{
        schemaVersion = 1
        patchId = "fetcher-zhi-compat"
        patchVersion = $PatchVersion
        sourceCommit = $SourceCommit.ToLowerInvariant()
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        files = @()
    }
    $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $stageRoot "fetcher-zhi-compat-patch.json") -Encoding UTF8

    if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
        Remove-Item -LiteralPath $archivePath -Force
    }
    Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $archivePath -CompressionLevel Optimal
}
finally {
    if (Test-Path -LiteralPath $stageRoot -PathType Container) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
}

Write-Host "Created: $archivePath"
Write-Host "SHA256: $((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant())"
