[CmdletBinding()]
param(
    [string] $InstallRoot = "",
    [string] $Repository = "Skooma-Breath/Fetcher-Simulator",
    [string] $ReleaseTag = "fetcher-tester-tools",
    [string] $AssetName = "fetcher-tester-tools.zip",
    [string] $GitHubApiBaseUrl = "https://api.github.com",
    [string] $GitHubDownloadBaseUrl = "https://github.com",
    [string] $ToolsArchivePath = "",
    [switch] $SkipUpdater
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$headers = @{ "User-Agent" = "Fetcher-Tester-Tools-Bootstrap" }
$scriptDirectory = $PSScriptRoot

function Select-InstallRoot {
    if (-not [string]::IsNullOrWhiteSpace($InstallRoot)) {
        return (Resolve-Path -LiteralPath $InstallRoot).Path
    }

    if (Test-Path -LiteralPath (Join-Path $scriptDirectory "openmw.exe") -PathType Leaf) {
        return (Resolve-Path -LiteralPath $scriptDirectory).Path
    }

    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Select your Fetcher Simulator folder (the folder containing openmw.exe)."
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw "No Fetcher Simulator folder was selected."
    }
    return (Resolve-Path -LiteralPath $dialog.SelectedPath).Path
}

function ConvertTo-SafeRelativePath {
    param([Parameter(Mandatory = $true)][string] $Path)

    if ([IO.Path]::IsPathRooted($Path)) {
        throw "Tester tools archive contains an absolute path: $Path"
    }
    $normalized = $Path.Replace("\", "/").TrimStart("/")
    $segments = @($normalized.Split("/", [StringSplitOptions]::RemoveEmptyEntries))
    if ($segments.Count -eq 0 -or $normalized.Contains(":")) {
        throw "Tester tools archive contains an invalid path: $Path"
    }
    foreach ($segment in $segments) {
        if ($segment -eq "." -or $segment -eq "..") {
            throw "Tester tools archive path escapes the install root: $Path"
        }
    }
    return ($segments -join "/")
}

$root = Select-InstallRoot
if (-not (Test-Path -LiteralPath (Join-Path $root "openmw.exe") -PathType Leaf)) {
    throw "The selected folder does not contain openmw.exe: $root"
}

$workRoot = Join-Path ([IO.Path]::GetTempPath()) ("fetcher-tester-bootstrap-" + [Guid]::NewGuid().ToString("N"))
$archivePath = Join-Path $workRoot $AssetName
$extractRoot = Join-Path $workRoot "extract"
try {
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    if ($ToolsArchivePath) {
        Copy-Item -LiteralPath $ToolsArchivePath -Destination $archivePath -Force
    }
    else {
        $encodedTag = [Uri]::EscapeDataString($ReleaseTag)
        $releaseUrl = "$($GitHubApiBaseUrl.TrimEnd('/'))/repos/$Repository/releases/tags/$encodedTag"
        $release = Invoke-RestMethod -UseBasicParsing -Uri $releaseUrl -Headers $headers
        $asset = @($release.assets | Where-Object { [string]$_.name -eq $AssetName })
        if ($asset.Count -ne 1 -or [string]$asset[0].digest -notmatch "^sha256:([0-9a-fA-F]{64})$") {
            throw "The $ReleaseTag release does not contain one digest-backed $AssetName asset."
        }
        $expectedHash = $Matches[1].ToLowerInvariant()
        $downloadUrl = "$($GitHubDownloadBaseUrl.TrimEnd('/'))/$Repository/releases/download/$encodedTag/$([Uri]::EscapeDataString($AssetName))"
        Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -Headers $headers -OutFile $archivePath
        $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Tester tools checksum mismatch. Expected $expectedHash, got $actualHash."
        }
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        foreach ($entry in $zip.Entries) {
            if (-not [string]::IsNullOrWhiteSpace($entry.FullName)) {
                [void](ConvertTo-SafeRelativePath -Path $entry.FullName.TrimEnd("/", "\"))
            }
        }
    }
    finally {
        $zip.Dispose()
    }
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot

    $manifestPath = Join-Path $extractRoot "fetcher-tester-tools.json"
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ([int]$manifest.schemaVersion -ne 1 -or [string]$manifest.channel -ne "fetcher-simulator-test") {
        throw "Unsupported Fetcher tester tools manifest."
    }
    foreach ($record in @($manifest.files | ForEach-Object { $_ })) {
        $relativePath = ConvertTo-SafeRelativePath -Path ([string]$record.path)
        $source = Join-Path $extractRoot $relativePath.Replace("/", "\")
        if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
            (Get-Item -LiteralPath $source).Length -ne [int64]$record.size -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne ([string]$record.sha256).ToLowerInvariant()) {
            throw "Tester tool failed manifest validation: $relativePath"
        }
        $destination = Join-Path $root $relativePath.Replace("/", "\")
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $root "fetcher-tester-tools.json") -Force
}
finally {
    if (Test-Path -LiteralPath $workRoot -PathType Container) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
}

Write-Host "Fetcher tester tools installed to: $root"
$modCompatibilityScript = Join-Path $root "Apply-Fetcher-ZHI-Compatibility.ps1"
if (Test-Path -LiteralPath $modCompatibilityScript -PathType Leaf) {
    & $modCompatibilityScript -InstallRoot $root
    if (-not $?) {
        throw "Fetcher client mod compatibility fixes failed."
    }
}
if (-not $SkipUpdater) {
    & (Join-Path $root "Update-Fetcher-Simulator.ps1") -InstallRoot $root
}
