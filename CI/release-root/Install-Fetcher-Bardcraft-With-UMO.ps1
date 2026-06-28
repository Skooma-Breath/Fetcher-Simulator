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
    [bool] $DownloadTes3cmdIfMissing = $true,
    [string] $SevenZipPath = "",
    [string] $SevenZipReleaseApiUrl = "https://api.github.com/repos/ip7z/7zip/releases/latest",
    [bool] $DownloadSevenZipIfMissing = $true,
    [bool] $RequireMorrowindData = $true,
    [bool] $ApplyBardcraftMultiplayerPatch = $true,
    [bool] $ApplyPublicTestConfig = $true,
    [string] $BardcraftPatchAssetName = "fetcher-bardcraft-mp-patch-v2.zip",
    [string] $BardcraftPatchUrl = "https://github.com/Skooma-Breath/Fetcher-Simulator/releases/download/fetcher-bardcraft-mp-patch-v2/fetcher-bardcraft-mp-patch-v2.zip",
    [string] $BardcraftPatchSha256 = "ced402accb7a1c09b57433f78500cb222a0a5703335b297ed02cfad422490128"
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

function Resolve-SevenZipDirectory {
    param([string] $Candidate)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Candidate)) {
        $candidates.Add($Candidate)
    }
    $candidates.Add((Join-Path $root "_fetcher_umo\7zip\7z.exe"))
    $candidates.Add((Join-Path $root "7z.exe"))
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $candidates.Add((Join-Path $env:ProgramFiles "7-Zip\7z.exe"))
    }
    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $candidates.Add((Join-Path ${env:ProgramFiles(x86)} "7-Zip\7z.exe"))
    }

    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            return (Split-Path -Parent (Resolve-Path -LiteralPath $path).Path)
        }
    }

    $command = Get-Command "7z.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return (Split-Path -Parent $command.Source)
    }

    if ($DownloadSevenZipIfMissing) {
        return Install-SevenZipIfMissing
    }

    throw "Could not find the full 7z.exe. Install 7-Zip, put 7z.exe on PATH, or run with -SevenZipPath C:\path\to\7z.exe."
}

function Assert-GitHubAssetChecksum {
    param(
        [Parameter(Mandatory = $true)] $Asset,
        [Parameter(Mandatory = $true)][string] $FilePath
    )

    $digest = [string] $Asset.digest
    if ([string]::IsNullOrWhiteSpace($digest) -or $digest -notmatch "^sha256:([0-9a-fA-F]{64})$") {
        Write-Warning "GitHub did not provide a SHA-256 digest for $($Asset.name); continuing after HTTPS download."
        return
    }

    $expectedHash = $Matches[1].ToLowerInvariant()
    $actualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Downloaded $($Asset.name) checksum mismatch. Expected $expectedHash but got $actualHash."
    }
}

function Install-SevenZipIfMissing {
    $downloadRoot = Join-Path $root "_fetcher_umo\7zip-bootstrap"
    $targetRoot = Join-Path $root "_fetcher_umo\7zip"
    $targetPath = Join-Path $targetRoot "7z.exe"
    $targetDll = Join-Path $targetRoot "7z.dll"
    if ((Test-Path -LiteralPath $targetPath -PathType Leaf) -and
        (Test-Path -LiteralPath $targetDll -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $targetRoot).Path
    }

    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    New-Item -ItemType Directory -Force -Path $targetRoot | Out-Null

    Write-Host "7-Zip with RAR support was not found. Downloading a portable copy..."
    Write-Host "  $SevenZipReleaseApiUrl"
    $headers = @{ "User-Agent" = "Fetcher-Simulator-Installer" }
    $release = Invoke-RestMethod -Uri $SevenZipReleaseApiUrl -Headers $headers
    $bootstrapAsset = @($release.assets) |
        Where-Object { $_.name -eq "7zr.exe" } |
        Select-Object -First 1
    $installerAsset = @($release.assets) |
        Where-Object { $_.name -match "^7z[0-9]+-x64\.exe$" } |
        Select-Object -First 1
    if (-not $bootstrapAsset -or -not $installerAsset) {
        throw "The latest official 7-Zip release does not contain the expected 7zr.exe and x64 installer assets."
    }

    $bootstrapPath = Join-Path $downloadRoot "7zr.exe"
    $installerPath = Join-Path $downloadRoot ([string] $installerAsset.name)
    Invoke-WebRequest -UseBasicParsing -Uri $bootstrapAsset.browser_download_url -Headers $headers -OutFile $bootstrapPath
    Invoke-WebRequest -UseBasicParsing -Uri $installerAsset.browser_download_url -Headers $headers -OutFile $installerPath
    Assert-GitHubAssetChecksum -Asset $bootstrapAsset -FilePath $bootstrapPath
    Assert-GitHubAssetChecksum -Asset $installerAsset -FilePath $installerPath

    & $bootstrapPath x $installerPath "-o$targetRoot" -y -aoa | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Portable 7-Zip extraction failed with exit code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $targetDll -PathType Leaf)) {
        throw "7-Zip extraction completed, but 7z.exe or 7z.dll was not found under $targetRoot."
    }

    Write-Host "Portable 7-Zip installed to: $targetRoot"
    return (Resolve-Path -LiteralPath $targetRoot).Path
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

function Test-UmoNxmHandler {
    param([Parameter(Mandatory = $true)][string] $UmoExecutable)

    $commandKey = "Registry::HKEY_CURRENT_USER\Software\Classes\nxm\shell\open\command"
    try {
        $command = [string] (Get-Item -LiteralPath $commandKey -ErrorAction Stop).GetValue("")
    }
    catch {
        return $false
    }

    if ([string]::IsNullOrWhiteSpace($command)) {
        return $false
    }

    $resolvedUmo = (Resolve-Path -LiteralPath $UmoExecutable).Path
    return $command.IndexOf($resolvedUmo, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
}

function Initialize-UmoProtocolHandler {
    param([Parameter(Mandatory = $true)][string] $UmoExecutable)

    if (Test-UmoNxmHandler -UmoExecutable $UmoExecutable) {
        return
    }

    Write-Host ""
    Write-Host "Registering this portable UMO as the Nexus download handler..."
    Invoke-Checked -Description "umo setup" -Command {
        "y" | & $UmoExecutable setup
    }

    if (-not (Test-UmoNxmHandler -UmoExecutable $UmoExecutable)) {
        throw "UMO setup completed, but Windows did not register this copy of umo.exe for nxm:// links."
    }
}

function Test-MorrowindDataConfigured {
    $cfgPath = Join-Path $root "openmw.cfg"
    if (-not (Test-Path -LiteralPath $cfgPath -PathType Leaf)) {
        return $false
    }

    foreach ($line in Get-Content -LiteralPath $cfgPath) {
        if ($line -notmatch "^\s*data\s*=\s*(.+?)\s*$") {
            continue
        }

        $dataPath = $Matches[1].Trim()
        if (($dataPath.StartsWith('"') -and $dataPath.EndsWith('"')) -or
            ($dataPath.StartsWith("'") -and $dataPath.EndsWith("'"))) {
            $dataPath = $dataPath.Substring(1, $dataPath.Length - 2)
        }
        $dataPath = [Environment]::ExpandEnvironmentVariables($dataPath)
        if (-not [System.IO.Path]::IsPathRooted($dataPath)) {
            $dataPath = Join-Path $root $dataPath
        }

        if (Test-Path -LiteralPath (Join-Path $dataPath "Morrowind.esm") -PathType Leaf) {
            return $true
        }
    }

    return $false
}

function Resolve-BardcraftDataRoot {
    $parsedMods = Get-Content -Raw -LiteralPath $ModListFile | ConvertFrom-Json
    if ($parsedMods -is [System.Array]) {
        $mods = @($parsedMods | ForEach-Object { $_ })
    }
    else {
        $mods = @($parsedMods)
    }
    $bardcraft = $mods |
        Where-Object { @($_.plugins) -contains "Bardcraft.ESP" } |
        Select-Object -First 1
    if (-not $bardcraft) {
        throw "The UMO modlist does not define the Bardcraft.ESP data root."
    }

    foreach ($dataPath in @($bardcraft.data_paths)) {
        if ([string]::IsNullOrWhiteSpace([string]$dataPath)) {
            continue
        }
        $candidate = Join-Path (Join-Path (Join-Path $UmoBasePath $ModListName) ([string]$bardcraft.category)) ([string]$dataPath)
        if ((Test-Path -LiteralPath (Join-Path $candidate "Bardcraft.ESP") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $candidate "scripts\Bardcraft") -PathType Container)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "UMO finished, but the installed Bardcraft data root could not be found under $UmoBasePath\$ModListName."
}

function Install-BardcraftMultiplayerPatch {
    param([Parameter(Mandatory = $true)][string] $BardcraftDataRoot)

    if ([string]::IsNullOrWhiteSpace($BardcraftPatchSha256)) {
        throw "BardcraftPatchSha256 must be set when the multiplayer patch is enabled."
    }

    $patchRoot = Join-Path $umoWorkRoot "bardcraft-mp-patch"
    $extractRoot = Join-Path $patchRoot "extracted"
    $zipPath = Join-Path $patchRoot $BardcraftPatchAssetName
    New-Item -ItemType Directory -Force -Path $patchRoot | Out-Null

    $expectedHash = $BardcraftPatchSha256.ToLowerInvariant()
    $downloadRequired = $true
    if (Test-Path -LiteralPath $zipPath -PathType Leaf) {
        $downloadRequired = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash
    }

    if ($downloadRequired) {
        Write-Host "Downloading Fetcher Bardcraft multiplayer patch:"
        Write-Host "  $BardcraftPatchUrl"
        Invoke-WebRequest -UseBasicParsing -Uri $BardcraftPatchUrl -OutFile $zipPath
    }

    $actualHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Downloaded Bardcraft patch checksum mismatch. Expected $expectedHash but got $actualHash."
    }

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force

    $appliers = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File -Filter "Apply-Fetcher-Bardcraft-MPPatch.ps1")
    if ($appliers.Count -ne 1) {
        throw "Expected exactly one Bardcraft patch applier in $zipPath, found $($appliers.Count)."
    }

    Write-Host "Applying Fetcher Bardcraft multiplayer compatibility patch..."
    & $appliers[0].FullName -BardcraftDataRoot $BardcraftDataRoot
}

if ($RequireMorrowindData -and -not (Test-MorrowindDataConfigured)) {
    throw "Morrowind.esm is not configured for this portable install. Run openmw-wizard.exe, point it at your Morrowind installation, then run this installer again."
}

$umo = Resolve-UmoPath $UmoPath
$tes3cmd = Resolve-Tes3cmdPath $Tes3cmdPath
$sevenZipDirectory = Resolve-SevenZipDirectory $SevenZipPath
$umoWorkRoot = Join-Path $root "_fetcher_umo"
$umoConfigDir = Join-Path $umoWorkRoot "config"
$umoCacheDir = Join-Path $umoWorkRoot "cache"
$umoConfigFile = Join-Path $umoConfigDir "config.json"
New-Item -ItemType Directory -Force -Path $umoConfigDir | Out-Null
New-Item -ItemType Directory -Force -Path $umoCacheDir | Out-Null

Write-Host "Using UMO: $umo"
Write-Host "Using tes3cmd: $tes3cmd"
Write-Host "Using 7-Zip: $sevenZipDirectory"
Write-Host "UMO mod install root: $UmoBasePath"
$env:PATH = "$sevenZipDirectory;$root;$env:PATH"
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
Initialize-UmoProtocolHandler -UmoExecutable $umo

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

if ($ApplyBardcraftMultiplayerPatch) {
    Write-Host ""
    $bardcraftDataRoot = Resolve-BardcraftDataRoot
    Install-BardcraftMultiplayerPatch -BardcraftDataRoot $bardcraftDataRoot
}

if ($ApplyPublicTestConfig) {
    $applyConfig = Join-Path $root "Apply-Fetcher-Public-Test-Config.ps1"
    if (Test-Path -LiteralPath $applyConfig) {
        Write-Host ""
        Write-Host "Applying Fetcher public test OpenMW load order..."
        & $applyConfig
    }
    else {
        Write-Warning "Could not find Apply-Fetcher-Public-Test-Config.ps1. Run Apply-Fetcher-Public-Test-Config.bat manually after installing mods."
    }
}

Write-Host ""
Write-Host "Done. If OpenMW still reports missing content, rerun Apply-Fetcher-Public-Test-Config.bat after UMO finishes extracting all mods."
