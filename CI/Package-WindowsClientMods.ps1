param(
    [Parameter(Mandatory = $true)]
    [string] $InstallDir,
    [string] $ModBundleUrl = "",
    [string] $ModBundleReleaseTag = "",
    [string] $ModBundleAsset = "openmw-client-mods.zip",
    [string] $Repository = $env:GITHUB_REPOSITORY,
    [bool] $IncludeTesterTools = $false,
    [string] $TesterToolsRepository = "Skooma-Breath/Fetcher-Updater",
    [string] $TesterToolsReleaseTag = "fetcher-tester-tools",
    [string] $TesterToolsAssetName = "fetcher-tester-tools.zip",
    [string] $TesterToolsSha256 = "",
    [string] $TesterToolsArchivePath = ""
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock] $Command,
        [Parameter(Mandatory = $true)]
        [string] $Description
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Copy-DirectoryContents {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Source,
        [Parameter(Mandatory = $true)]
        [string] $Destination
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        return
    }

    New-Item -ItemType Directory -Force $Destination | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
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

function Install-TesterToolsArchive {
    param(
        [Parameter(Mandatory = $true)][string] $ArchivePath,
        [Parameter(Mandatory = $true)][string] $DestinationRoot,
        [Parameter(Mandatory = $true)][string] $WorkRoot
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $archivePaths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $trimmedPath = $entry.FullName.TrimEnd("/", "\")
            if ([string]::IsNullOrWhiteSpace($trimmedPath)) {
                continue
            }
            $relativePath = ConvertTo-SafeRelativePath -Path $trimmedPath
            if (-not $archivePaths.Add($relativePath)) {
                throw "Tester tools archive contains a duplicate path: $relativePath"
            }
        }
    }
    finally {
        $zip.Dispose()
    }

    $testerToolsExtractRoot = Join-Path $WorkRoot "tester-tools"
    New-Item -ItemType Directory -Force -Path $testerToolsExtractRoot | Out-Null
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $testerToolsExtractRoot
    $manifestPath = Join-Path $testerToolsExtractRoot "fetcher-tester-tools.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Tester tools archive does not contain fetcher-tester-tools.json."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int]$manifest.schemaVersion -ne 1 -or
        [string]$manifest.channel -ne "fetcher-simulator-test" -or
        [string]$manifest.sourceCommit -notmatch "^[0-9a-fA-F]{40}$" -or
        $null -eq $manifest.PSObject.Properties["files"] -or
        @($manifest.files).Count -eq 0) {
        throw "Unsupported Fetcher tester tools manifest."
    }

    $manifestPaths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($manifest.files)) {
        $relativePath = ConvertTo-SafeRelativePath -Path ([string]$record.path)
        $expectedHash = [string]$record.sha256
        if ($relativePath.Equals("fetcher-tester-tools.json", [StringComparison]::OrdinalIgnoreCase) -or
            -not $manifestPaths.Add($relativePath) -or [int64]$record.size -lt 0 -or
            $expectedHash -notmatch "^[0-9a-fA-F]{64}$") {
            throw "Tester tools manifest contains an invalid record: $relativePath"
        }
        $sourcePath = Join-Path $testerToolsExtractRoot $relativePath.Replace("/", "\")
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf) -or
            (Get-Item -LiteralPath $sourcePath).Length -ne [int64]$record.size -or
            (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash.ToLowerInvariant()) {
            throw "Tester tool failed manifest validation: $relativePath"
        }
    }

    $payloadPaths = @(Get-ChildItem -LiteralPath $testerToolsExtractRoot -File -Recurse | ForEach-Object {
        $_.FullName.Substring($testerToolsExtractRoot.Length).TrimStart("\", "/").Replace("\", "/")
    } | Where-Object { -not $_.Equals("fetcher-tester-tools.json", [StringComparison]::OrdinalIgnoreCase) })
    if ($payloadPaths.Count -ne $manifestPaths.Count) {
        throw "Tester tools manifest does not cover the complete archive payload."
    }
    foreach ($payloadPath in $payloadPaths) {
        if (-not $manifestPaths.Contains($payloadPath)) {
            throw "Tester tools archive contains an unmanifested payload: $payloadPath"
        }
    }

    foreach ($record in @($manifest.files)) {
        $relativePath = ConvertTo-SafeRelativePath -Path ([string]$record.path)
        $sourcePath = Join-Path $testerToolsExtractRoot $relativePath.Replace("/", "\")
        $destinationPath = Join-Path $DestinationRoot $relativePath.Replace("/", "\")
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationPath) | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $DestinationRoot "fetcher-tester-tools.json") -Force
}

$installPath = (Resolve-Path -LiteralPath $InstallDir).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$workDir = Join-Path $installPath "_client_mod_package"
$extractDir = Join-Path $workDir "extract"
$bundlePath = Join-Path $workDir $ModBundleAsset
New-Item -ItemType Directory -Force $workDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $installPath "Data Files") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $installPath "userdata") | Out-Null

if ($ModBundleUrl) {
    Write-Output "Downloading mod bundle from $ModBundleUrl..."
    Invoke-WebRequest -Uri $ModBundleUrl -OutFile $bundlePath
}
elseif ($ModBundleReleaseTag) {
    if (-not $Repository) {
        throw "Repository is required when ModBundleReleaseTag is set."
    }
    $directUrl = "https://github.com/$Repository/releases/download/$ModBundleReleaseTag/$ModBundleAsset"
    Write-Output "Downloading mod bundle $ModBundleAsset from release $ModBundleReleaseTag..."
    try {
        Invoke-WebRequest -Uri $directUrl -OutFile $bundlePath
    }
    catch {
        Write-Warning "Direct release asset download failed: $($_.Exception.Message)"
        if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
            throw "GitHub CLI 'gh' is required to download mod bundle release assets when direct download fails."
        }
        Invoke-Checked -Description "gh release download" -Command {
            gh release download $ModBundleReleaseTag --repo $Repository --pattern $ModBundleAsset --dir $workDir --clobber
        }
    }
}

$manifest = [ordered]@{
    fallbackArchives = @("Morrowind.bsa", "Tribunal.bsa", "Bloodmoon.bsa")
    dataDirs = @("./Data Files")
    content = @("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm")
    userData = "./userdata"
}

if (Test-Path -LiteralPath $bundlePath) {
    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force $extractDir | Out-Null
    Expand-Archive -LiteralPath $bundlePath -DestinationPath $extractDir -Force

    $manifestCandidates = @(
        (Join-Path $extractDir "openmw-client-package.json"),
        (Join-Path $extractDir "openmw-package.json")
    )
    $manifestPath = $manifestCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($manifestPath) {
        $manifestJson = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        foreach ($property in @("fallbackArchives", "dataDirs", "content", "userData")) {
            if ($manifestJson.PSObject.Properties.Name -contains $property) {
                $manifest[$property] = $manifestJson.$property
            }
        }
    }

    if (Test-Path -LiteralPath (Join-Path $extractDir "Data Files")) {
        Copy-DirectoryContents -Source (Join-Path $extractDir "Data Files") -Destination (Join-Path $installPath "Data Files")
    }
    else {
        Copy-DirectoryContents -Source $extractDir -Destination (Join-Path $installPath "Data Files")
    }
    foreach ($candidate in $manifestCandidates) {
        $copiedManifest = Join-Path $installPath (Split-Path -Leaf $candidate)
        if (Test-Path -LiteralPath $copiedManifest) {
            Remove-Item -LiteralPath $copiedManifest -Force
        }
    }
}

$menuTextureSources = @(
    (Join-Path $installPath "resources\vfs\textures"),
    (Join-Path $repoRoot "files\data\textures")
)
$menuTextureDest = Join-Path $installPath "Data Files\textures"
New-Item -ItemType Directory -Force $menuTextureDest | Out-Null
foreach ($texture in @("menu_mainmenu.dds", "menu_mainmenu_over.dds", "menu_mainmenu_pressed.dds")) {
    $source = $menuTextureSources |
        ForEach-Object { Join-Path $_ $texture } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if ($source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $menuTextureDest $texture) -Force
    }
    else {
        Write-Warning "Could not find $texture in any known OpenMW texture source."
    }
}

$openmwCfg = Join-Path $installPath "openmw.cfg"
$existingLines = @()
if (Test-Path -LiteralPath $openmwCfg) {
    $existingLines = Get-Content -LiteralPath $openmwCfg
}

$portableKeysPattern = "^(config|replace|user-data|resources|data|data-local|content|fallback-archive)="
$preservedLines = @($existingLines | Where-Object { $_ -notmatch $portableKeysPattern })
$userConfigPath = Join-Path $installPath $manifest.userData
New-Item -ItemType Directory -Force $userConfigPath | Out-Null

$configLines = New-Object System.Collections.Generic.List[string]
$configLines.Add("# Portable Fetcher config generated by CI.")
$configLines.Add("# This file intentionally ignores Documents\My Games\OpenMW.")
$configLines.Add("replace=config")
$configLines.Add("user-data=$($manifest.userData)")
$configLines.Add("resources=./resources")
$configLines.Add("data=./resources/vfs-mw")
foreach ($dataDir in @($manifest.dataDirs)) {
    if ($dataDir) {
        $configLines.Add("data=$dataDir")
    }
}
foreach ($archive in @($manifest.fallbackArchives)) {
    if ($archive) {
        $configLines.Add("fallback-archive=$archive")
    }
}
foreach ($content in @($manifest.content)) {
    if ($content) {
        $configLines.Add("content=$content")
    }
}
$configLines.Add("")
if ($preservedLines.Count -gt 0) {
    $configLines.AddRange([string[]]$preservedLines)
}
Set-Content -LiteralPath $openmwCfg -Value $configLines -Encoding ASCII

$settingsLines = @(
    "# Portable Fetcher settings generated by CI.",
    "# This file intentionally ignores Documents\My Games\OpenMW.",
    "",
    "[Camera]",
    "viewing distance = 28672.0",
    "",
    "[Terrain]",
    "distant terrain = true",
    "object paging = true",
    "object paging active grid = true",
    "",
    "[Fog]",
    "use distant fog = false",
    "",
    "[Game]",
    "shield sheathing = true",
    "smooth animation transitions = true",
    "use additional anim sources = true",
    "weapon sheathing = true",
    "",
    "[Lua]",
    "lua profiler = false",
    "",
    "[Video]",
    "antialiasing = 16"
)
$settingsCfg = Join-Path $installPath "settings.cfg"
Set-Content -LiteralPath $settingsCfg -Encoding ASCII -Value $settingsLines

$userSettingsCfg = Join-Path $userConfigPath "settings.cfg"
Set-Content -LiteralPath $userSettingsCfg -Encoding ASCII -Value $settingsLines

$channelManifest = [ordered]@{
    schemaVersion = 1
    channel = if ($IncludeTesterTools) { "test" } else { "clean" }
}
$channelManifest | ConvertTo-Json -Depth 3 |
    Set-Content -LiteralPath (Join-Path $installPath "fetcher-client-channel.json") -Encoding UTF8

if ($IncludeTesterTools) {
    $testerToolsArchive = $TesterToolsArchivePath
    if ([string]::IsNullOrWhiteSpace($testerToolsArchive)) {
        if ($TesterToolsSha256 -notmatch "^[0-9a-fA-F]{64}$") {
            throw "TesterToolsSha256 must pin the remote fetcher-tester-tools.zip digest."
        }
        $releaseUrl = "https://api.github.com/repos/$TesterToolsRepository/releases/tags/$([Uri]::EscapeDataString($TesterToolsReleaseTag))"
        $release = Invoke-RestMethod -UseBasicParsing -Headers @{ "User-Agent" = "Fetcher-Simulator-Packager" } -Uri $releaseUrl
        $assets = @($release.assets | Where-Object { [string]$_.name -eq $TesterToolsAssetName })
        $expectedDigest = "sha256:$($TesterToolsSha256.ToLowerInvariant())"
        if ($assets.Count -ne 1 -or
            -not ([string]$assets[0].digest).Equals($expectedDigest, [StringComparison]::OrdinalIgnoreCase)) {
            throw "The pinned tester-tools digest does not match the GitHub release asset."
        }
        $testerToolsArchive = Join-Path $workDir $TesterToolsAssetName
        Invoke-WebRequest -UseBasicParsing -Headers @{ "User-Agent" = "Fetcher-Simulator-Packager" } `
            -Uri ([string]$assets[0].browser_download_url) -OutFile $testerToolsArchive
    }
    else {
        $testerToolsArchive = (Resolve-Path -LiteralPath $testerToolsArchive).Path
    }
    $actualTesterToolsHash = (Get-FileHash -LiteralPath $testerToolsArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not [string]::IsNullOrWhiteSpace($TesterToolsSha256) -and
        $actualTesterToolsHash -ne $TesterToolsSha256.ToLowerInvariant()) {
        throw "Tester tools archive checksum mismatch. Expected $TesterToolsSha256, got $actualTesterToolsHash."
    }
    Install-TesterToolsArchive -ArchivePath $testerToolsArchive -DestinationRoot $installPath -WorkRoot $workDir
}

$readmePath = Join-Path $installPath "PACKAGE_README.txt"
Set-Content -LiteralPath $readmePath -Encoding ASCII -Value @(
    $(if ($IncludeTesterTools) { "Fetcher Simulator test-channel package" } else { "Fetcher Simulator clean package" }),
    "",
    "This package is portable. It uses the openmw.cfg next to openmw.exe and intentionally ignores Documents\My Games\OpenMW\openmw.cfg.",
    "Writable config, logs, saves, screenshots, and generated user files are stored under .\userdata.",
    "Load order and content entries are stored directly in .\openmw.cfg so the package works without per-user OpenMW config.",
    "Portable graphics defaults are seeded in settings.cfg and .\userdata\settings.cfg. Launcher changes are saved to .\userdata\settings.cfg.",
    $(if ($IncludeTesterTools) {
        "For later test-channel releases, close OpenMW and run .\Update-Fetcher-Simulator.bat. It preserves portable config and player data."
    } else {
        "This clean package intentionally excludes Fetcher tester tools and bundled gameplay mods. To opt in, download Join-Fetcher-Test-Channel.bat from https://github.com/Skooma-Breath/Fetcher-Updater/releases/tag/fetcher-tester-tools."
    }),
    "",
    "Bundled mod files live under .\Data Files unless the package manifest declares additional data directories."
)

Remove-Item -LiteralPath $workDir -Recurse -Force
Write-Output "Portable OpenMW config written to $openmwCfg."
Write-Output "Portable OpenMW settings written to $settingsCfg."
Write-Output "Portable OpenMW writable settings written to $userSettingsCfg."
