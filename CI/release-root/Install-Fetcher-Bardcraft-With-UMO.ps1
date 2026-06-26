param(
    [string] $UmoPath = "",
    [string] $ModListName = "fetcher-bardcraft",
    [string] $ModListFile = "",
    [string] $ModListAssetName = "fetcher-bardcraft-umo.json",
    [string] $ModListUrl = "https://github.com/Skooma-Breath/Fetcher-Simulator/releases/download/Fetcher-Simulator/fetcher-bardcraft-umo.json",
    [string] $UmoBasePath = "",
    [bool] $DownloadUmoIfMissing = $true,
    [string] $Tes3cmdPath = "",
    [string] $Tes3cmdUrl = "https://gitlab.com/modding-openmw/tes3cmd/-/jobs/artifacts/master/raw/tes3cmd.0.40-PRE-RELEASE-2-win.zip?job=build_win",
    [bool] $DownloadTes3cmdIfMissing = $true
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($UmoBasePath)) {
    $UmoBasePath = Join-Path $root "Data Files"
}
New-Item -ItemType Directory -Force -Path $UmoBasePath | Out-Null
$UmoBasePath = (Resolve-Path -LiteralPath $UmoBasePath).Path

function Resolve-UmoPath {
    param([string] $Candidate)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Candidate)) {
        $candidates.Add($Candidate)
    }
    $candidates.Add((Join-Path $root "umo.exe"))
    $candidates.Add((Join-Path $root "umo\umo.exe"))
    $candidates.Add((Join-Path $root "tools\umo\umo.exe"))

    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    $command = Get-Command "umo.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $command = Get-Command "umo" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    if ($DownloadUmoIfMissing) {
        return Install-UmoIfMissing
    }

    throw "Could not find umo.exe. Put umo.exe next to this BAT file, add it to PATH, or run with -UmoPath C:\path\to\umo.exe."
}

function Install-UmoIfMissing {
    $targetPath = Join-Path $root "umo.exe"
    if (Test-Path -LiteralPath $targetPath) {
        return (Resolve-Path -LiteralPath $targetPath).Path
    }

    $downloadRoot = Join-Path $root "_fetcher_umo"
    $extractRoot = Join-Path $downloadRoot "umo-win"
    $zipPath = Join-Path $downloadRoot "umo-win.zip"
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null

    function ConvertTo-FlatArray {
        param($Value)

        if ($null -eq $Value) {
            return @()
        }

        if ($Value -is [System.Array]) {
            return @($Value | ForEach-Object { $_ })
        }

        return @($Value)
    }

    $packagesUri = "https://gitlab.com/api/v4/projects/modding-openmw%2Fumo/packages?package_name=umo&sort=desc&per_page=5"
    Write-Host "umo.exe was not found. Downloading UMO for Windows..."
    Write-Host "  $packagesUri"

    $packages = @(ConvertTo-FlatArray (Invoke-RestMethod -Uri $packagesUri))
    $package = $packages |
        Where-Object { $_.package_type -eq "generic" -and $_.status -eq "default" } |
        Select-Object -First 1
    if (-not $package) {
        throw "Could not find a current UMO generic package from GitLab."
    }

    [long] $packageId = 0
    if (-not [long]::TryParse(([string] $package.id), [ref] $packageId)) {
        throw "GitLab returned an invalid UMO package id: $($package.id)"
    }
    $packageVersion = [string] $package.version
    if ([string]::IsNullOrWhiteSpace($packageVersion)) {
        throw "GitLab returned UMO package $packageId without a version."
    }

    $packageFile = $null
    $packageFilesUri = "https://gitlab.com/api/v4/projects/modding-openmw%2Fumo/packages/$packageId/package_files?per_page=100"
    try {
        $packageFiles = @(ConvertTo-FlatArray (Invoke-RestMethod -Uri $packageFilesUri))
        $packageFile = $packageFiles |
            Where-Object { $_.file_name -eq "umo-win.zip" } |
            Select-Object -First 1
    }
    catch {
        Write-Warning "Could not query UMO package files by id; falling back to GitLab generic package download. Details: $($_.Exception.Message)"
    }

    if ($packageFile -and $packageFile.id) {
        $downloadUri = "https://gitlab.com/modding-openmw/umo/-/package_files/$($packageFile.id)/download"
    }
    else {
        $escapedVersion = [System.Uri]::EscapeDataString($packageVersion)
        $downloadUri = "https://gitlab.com/api/v4/projects/modding-openmw%2Fumo/packages/generic/umo/$escapedVersion/umo-win.zip"
    }

    Write-Host "Downloading UMO ${packageVersion}:"
    Write-Host "  $downloadUri"
    Invoke-WebRequest -UseBasicParsing -Uri $downloadUri -OutFile $zipPath

    if ($packageFile -and -not [string]::IsNullOrWhiteSpace($packageFile.file_sha256)) {
        $actualHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $expectedHash = ([string]$packageFile.file_sha256).ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Downloaded UMO checksum mismatch. Expected $expectedHash but got $actualHash."
        }
    }

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force

    $extractedUmo = Get-ChildItem -LiteralPath $extractRoot -Recurse -File -Filter "umo.exe" |
        Select-Object -First 1
    if (-not $extractedUmo) {
        throw "UMO downloaded, but umo.exe was not found inside $zipPath."
    }

    Copy-Item -LiteralPath $extractedUmo.FullName -Destination $targetPath -Force
    Write-Host "UMO installed to: $targetPath"
    return (Resolve-Path -LiteralPath $targetPath).Path
}

function Resolve-Tes3cmdPath {
    param([string] $Candidate)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Candidate)) {
        $candidates.Add($Candidate)
    }
    $candidates.Add((Join-Path $root "tes3cmd.exe"))
    $candidates.Add((Join-Path $root "tes3cmd\tes3cmd.exe"))
    $candidates.Add((Join-Path $root "tools\tes3cmd\tes3cmd.exe"))

    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    $command = Get-Command "tes3cmd.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $command = Get-Command "tes3cmd" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    if ($DownloadTes3cmdIfMissing) {
        return Install-Tes3cmdIfMissing
    }

    throw "Could not find tes3cmd.exe. Put tes3cmd.exe next to this BAT file, add it to PATH, or run with -Tes3cmdPath C:\path\to\tes3cmd.exe."
}

function Install-Tes3cmdIfMissing {
    $targetPath = Join-Path $root "tes3cmd.exe"
    if (Test-Path -LiteralPath $targetPath) {
        return (Resolve-Path -LiteralPath $targetPath).Path
    }

    $downloadRoot = Join-Path $root "_fetcher_umo"
    $extractRoot = Join-Path $downloadRoot "tes3cmd-win"
    $zipPath = Join-Path $downloadRoot "tes3cmd-win.zip"
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null

    Write-Host "tes3cmd.exe was not found. Downloading tes3cmd for Windows..."
    Write-Host "  $Tes3cmdUrl"
    Invoke-WebRequest -UseBasicParsing -Uri $Tes3cmdUrl -OutFile $zipPath

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force

    $extractedTes3cmd = Get-ChildItem -LiteralPath $extractRoot -Recurse -File -Filter "tes3cmd*.exe" |
        Select-Object -First 1
    if (-not $extractedTes3cmd) {
        throw "tes3cmd downloaded, but no tes3cmd executable was found inside $zipPath."
    }

    Copy-Item -LiteralPath $extractedTes3cmd.FullName -Destination $targetPath -Force
    Write-Host "tes3cmd installed to: $targetPath"
    return (Resolve-Path -LiteralPath $targetPath).Path
}

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

$umo = Resolve-UmoPath $UmoPath
$tes3cmd = Resolve-Tes3cmdPath $Tes3cmdPath
$umoWorkRoot = Join-Path $root "_fetcher_umo"
$umoConfigDir = Join-Path $umoWorkRoot "config"
$umoCacheDir = Join-Path $umoWorkRoot "cache"
$umoConfigFile = Join-Path $umoConfigDir "config.json"
New-Item -ItemType Directory -Force -Path $umoConfigDir | Out-Null
New-Item -ItemType Directory -Force -Path $umoCacheDir | Out-Null

Write-Host "Using UMO: $umo"
Write-Host "Using tes3cmd: $tes3cmd"
Write-Host "UMO mod install root: $UmoBasePath"
$env:PATH = "$root;$env:PATH"
$env:UMO_BASEPATH = $UmoBasePath
$env:UMO_CACHE_DIR = $umoCacheDir
$env:UMO_CONF_DIR = $umoConfigDir
$env:UMO_CONF_FILE = $umoConfigFile
$env:UMO_TES3CMD = $tes3cmd

if ([string]::IsNullOrWhiteSpace($ModListFile)) {
    $localModListCandidates = @(
        (Join-Path $root $ModListAssetName),
        (Join-Path $root "$ModListName.json")
    )
    foreach ($localModList in $localModListCandidates) {
        if (Test-Path -LiteralPath $localModList) {
            $ModListFile = $localModList
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ModListFile)) {
    $cacheDir = Join-Path $root "_fetcher_umo"
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    $ModListFile = Join-Path $cacheDir $ModListAssetName

    Write-Host "Downloading UMO modlist:"
    Write-Host "  $ModListUrl"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $ModListUrl -OutFile $ModListFile
    }
    catch {
        throw "Could not download $ModListUrl. The Fetcher Bardcraft UMO modlist may not be published yet. You can also place $ModListAssetName next to this BAT and run it again. Details: $($_.Exception.Message)"
    }
}

if (-not (Test-Path -LiteralPath $ModListFile)) {
    throw "Could not find UMO modlist file: $ModListFile"
}

$ModListFile = (Resolve-Path -LiteralPath $ModListFile).Path
Write-Host "Using modlist: $ModListFile"
Write-Host ""
Write-Host "Checking UMO setup..."
Invoke-Checked -Description "umo check" -Command {
    if (Test-Path -LiteralPath $umoConfigFile) {
        & $umo check
    }
    else {
        $firstRunAnswers = @(
            $UmoBasePath,
            $umoCacheDir
        )
        $firstRunAnswers | & $umo check
    }
}

Write-Host ""
Write-Host "Registering UMO modlist..."
Invoke-Checked -Description "umo list add" -Command {
    & $umo list add $ModListFile --list-name $ModListName
}

Write-Host ""
Write-Host "Syncing UMO mod metadata..."
Invoke-Checked -Description "umo sync $ModListName" -Command {
    & $umo sync $ModListName --skip-momw
}

Write-Host ""
Write-Host "Installing UMO modlist. Non-premium Nexus users may need to click the Nexus download pages that UMO opens."
Write-Host "Large Nexus downloads may look quiet in this console while UMO is working. Let this window keep running."
Write-Host "Mods will be extracted under: $UmoBasePath\$ModListName"
Invoke-Checked -Description "umo install $ModListName" -Command {
    & $umo install $ModListName
}

$applyConfig = Join-Path $root "Apply-Fetcher-Public-Test-Config.ps1"
if (Test-Path -LiteralPath $applyConfig) {
    Write-Host ""
    Write-Host "Applying Fetcher public test OpenMW load order..."
    & $applyConfig
}
else {
    Write-Warning "Could not find Apply-Fetcher-Public-Test-Config.ps1. Run Apply-Fetcher-Public-Test-Config.bat manually after installing mods."
}

Write-Host ""
Write-Host "Done. If OpenMW still reports missing content, rerun Apply-Fetcher-Public-Test-Config.bat after UMO finishes extracting all mods."
