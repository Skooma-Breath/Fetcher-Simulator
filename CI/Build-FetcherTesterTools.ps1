param(
    [string] $SourceDir = "",
    [Parameter(Mandatory = $true)][string] $OutputDir,
    [string] $SourceCommit = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = Join-Path $PSScriptRoot "release-root"
}

$files = @(
    "Apply-Fetcher-Public-Test-Config.bat",
    "Apply-Fetcher-Public-Test-Config.ps1",
    "Apply-Fetcher-ZHI-Compatibility.ps1",
    "fetcher-bardcraft-umo.json",
    "fetcher-client-patches.json",
    "FETCHER_SIMULATOR_README.txt",
    "Install-Fetcher-Bardcraft-With-UMO.bat",
    "Install-Fetcher-Bardcraft-With-UMO.ps1",
    "Install-Fetcher-Tester-Tools.ps1",
    "Join-Fetcher-Test-Channel.bat",
    "Launch-Fetcher-Character.bat",
    "Launch-Fetcher-Character.ps1",
    "Update-Fetcher-Simulator.bat",
    "Update-Fetcher-Simulator.ps1"
)

if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    throw "Tester tools source directory was not found: $SourceDir"
}
if ($SourceCommit -and $SourceCommit -notmatch "^[0-9a-fA-F]{40}$") {
    throw "SourceCommit must be empty or a full 40-character Git commit hash."
}

$outputPath = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$stage = Join-Path ([IO.Path]::GetTempPath()) ("fetcher-tester-tools-" + [Guid]::NewGuid().ToString("N"))
$archive = Join-Path $outputPath "fetcher-tester-tools.zip"

try {
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($relativePath in $files) {
        $source = Join-Path $SourceDir $relativePath
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Required tester tool was not found: $source"
        }
        $destination = Join-Path $stage $relativePath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
        $item = Get-Item -LiteralPath $destination
        $records.Add([ordered]@{
            path = $relativePath.Replace("\", "/")
            size = [int64]$item.Length
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }

    $manifest = [ordered]@{
        schemaVersion = 1
        channel = "fetcher-simulator-test"
        sourceCommit = $SourceCommit.ToLowerInvariant()
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        files = @($records | ForEach-Object { $_ })
    }
    $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $stage "fetcher-tester-tools.json") -Encoding UTF8

    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $archive -CompressionLevel Optimal
    Copy-Item -LiteralPath (Join-Path $SourceDir "Install-Fetcher-Tester-Tools.ps1") `
        -Destination (Join-Path $outputPath "Install-Fetcher-Tester-Tools.ps1") -Force
    Copy-Item -LiteralPath (Join-Path $SourceDir "Join-Fetcher-Test-Channel.bat") `
        -Destination (Join-Path $outputPath "Join-Fetcher-Test-Channel.bat") -Force
    Copy-Item -LiteralPath (Join-Path $SourceDir "fetcher-bardcraft-umo.json") `
        -Destination (Join-Path $outputPath "fetcher-bardcraft-umo.json") -Force
}
finally {
    if (Test-Path -LiteralPath $stage -PathType Container) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}

Write-Host "Fetcher tester tools written: $archive"
