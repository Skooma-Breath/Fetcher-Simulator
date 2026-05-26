param(
    [Parameter(Mandatory = $true)]
    [string] $InstallDir,
    [string] $ModBundleUrl = "",
    [string] $ModBundleReleaseTag = "",
    [string] $ModBundleAsset = "openmw-client-mods.zip",
    [string] $Repository = $env:GITHUB_REPOSITORY
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
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "GitHub CLI 'gh' is required to download mod bundle release assets."
    }
    Write-Output "Downloading mod bundle $ModBundleAsset from release $ModBundleReleaseTag..."
    Invoke-Checked -Description "gh release download" -Command {
        gh release download $ModBundleReleaseTag --repo $Repository --pattern $ModBundleAsset --dir $workDir --clobber
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

$configLines = New-Object System.Collections.Generic.List[string]
$configLines.Add("# Portable tester config generated by CI.")
$configLines.Add("# This file intentionally ignores Documents\My Games\OpenMW.")
$configLines.Add("replace=config")
$configLines.Add("config=$($manifest.userData)")
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
    "# Portable tester settings generated by CI.",
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
    "use distant fog = false"
)
$settingsCfg = Join-Path $installPath "settings.cfg"
Set-Content -LiteralPath $settingsCfg -Encoding ASCII -Value $settingsLines

$userConfigPath = Join-Path $installPath $manifest.userData
New-Item -ItemType Directory -Force $userConfigPath | Out-Null
$userSettingsCfg = Join-Path $userConfigPath "settings.cfg"
Set-Content -LiteralPath $userSettingsCfg -Encoding ASCII -Value $settingsLines

$readmePath = Join-Path $installPath "TESTER_PACKAGE_README.txt"
Set-Content -LiteralPath $readmePath -Encoding ASCII -Value @(
    "OpenMW tester package",
    "",
    "This package uses the openmw.cfg next to openmw.exe.",
    "Writable config, logs, saves, screenshots, and generated user files are written under .\userdata.",
    "Portable graphics defaults are seeded in settings.cfg and .\userdata\settings.cfg.",
    "Launcher changes are saved to .\userdata\settings.cfg.",
    "It intentionally does not read Documents\My Games\OpenMW\openmw.cfg.",
    "",
    "Bundled mod files live under .\Data Files unless the package manifest declares additional data directories.",
    "Do not upload proprietary game data unless you have the rights to distribute it."
)

Remove-Item -LiteralPath $workDir -Recurse -Force
Write-Output "Portable OpenMW tester config written to $openmwCfg."
Write-Output "Portable OpenMW tester settings written to $settingsCfg."
Write-Output "Portable OpenMW writable settings written to $userSettingsCfg."
