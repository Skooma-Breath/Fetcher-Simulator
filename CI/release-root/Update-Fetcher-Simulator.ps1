[CmdletBinding()]
param(
    [string] $InstallRoot = "",
    [string] $Repository = "Skooma-Breath/Fetcher-Simulator",
    [string] $GitHubApiBaseUrl = "https://api.github.com",
    [string] $GitHubDownloadBaseUrl = "https://github.com",
    [string] $ClientReleaseTag = "Fetcher-Simulator-Test",
    [string] $ClientAssetName = "fetcher-simulator-test.zip",
    [string] $TesterToolsReleaseTag = "fetcher-tester-tools",
    [string] $TesterToolsAssetName = "fetcher-tester-tools.zip",
    [string] $TesterToolsArchivePath = "",
    [string] $PatchCatalogPath = "",
    [string] $UmoInstallerPath = "",
    [switch] $SkipClientUpdate,
    [switch] $SkipTesterToolsUpdate,
    [switch] $SkipUmoMods,
    [switch] $SkipModPatches
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$headers = @{ "User-Agent" = "Fetcher-Simulator-Updater" }

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-WithRetry {
    param(
        [Parameter(Mandatory = $true)][scriptblock] $Action,
        [Parameter(Mandatory = $true)][string] $Description,
        [int] $Attempts = 3
    )

    for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
        try {
            return & $Action
        }
        catch {
            if ($attempt -eq $Attempts) {
                throw
            }
            Write-Warning "$Description failed (attempt $attempt of $Attempts): $($_.Exception.Message)"
            Start-Sleep -Seconds (2 * $attempt)
        }
    }
}

function ConvertTo-NormalizedRelativePath {
    param([Parameter(Mandatory = $true)][string] $RelativePath)

    if ([IO.Path]::IsPathRooted($RelativePath)) {
        throw "Update manifest contains an absolute path: $RelativePath"
    }

    $path = $RelativePath.Replace("\", "/").TrimStart("/")
    $segments = @($path.Split("/", [StringSplitOptions]::RemoveEmptyEntries))
    if ($segments.Count -eq 0 -or $path.Contains(":")) {
        throw "Update manifest contains an invalid path: $RelativePath"
    }
    foreach ($segment in $segments) {
        if ($segment -eq "." -or $segment -eq "..") {
            throw "Update manifest path escapes the install root: $RelativePath"
        }
    }
    return ($segments -join "/")
}

function Test-FetcherMutablePath {
    param([Parameter(Mandatory = $true)][string] $RelativePath)

    $path = $RelativePath.Replace("\", "/").TrimStart("/").ToLowerInvariant()
    if (@(
        "fetcher-update-state.json",
        "openmw.cfg",
        "settings.cfg",
        "server.cfg",
        "launcher.cfg",
        "openmw-launcher.cfg",
        "playerdata.db",
        "server-lua-storage.bin",
        "umo.exe",
        "tes3cmd.exe",
        "apply-fetcher-public-test-config.bat",
        "apply-fetcher-public-test-config.ps1",
        "apply-fetcher-zhi-compatibility.ps1",
        "fetcher-bardcraft-umo.json",
        "fetcher-client-patches.json",
        "fetcher-tester-tools.json",
        "fetcher_simulator_readme.txt",
        "install-fetcher-bardcraft-with-umo.bat",
        "install-fetcher-bardcraft-with-umo.ps1",
        "install-fetcher-tester-tools.ps1",
        "join-fetcher-test-channel.bat",
        "launch-fetcher-character.bat",
        "launch-fetcher-character.ps1",
        "update-fetcher-simulator.bat",
        "update-fetcher-simulator.ps1"
    ) -contains $path) {
        return $true
    }

    foreach ($prefix in @(
        "_fetcher_umo/",
        "_fetcher_update/",
        "bardcraft/",
        "logs/",
        "mp-keys/",
        "profiles/",
        "saves/",
        "screenshots/",
        "userdata/"
    )) {
        if ($path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }

    return $path.EndsWith(".dmp", [StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeArchivePaths {
    param([Parameter(Mandatory = $true)][string] $ArchivePath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrWhiteSpace($entry.FullName)) {
                continue
            }
            [void](ConvertTo-NormalizedRelativePath -RelativePath $entry.FullName.TrimEnd("/", "\"))
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Get-GitHubRelease {
    param([Parameter(Mandatory = $true)][string] $Tag)

    $encodedTag = [Uri]::EscapeDataString($Tag)
    $url = "$($GitHubApiBaseUrl.TrimEnd('/'))/repos/$Repository/releases/tags/$encodedTag"
    $response = Invoke-WithRetry -Description "Reading GitHub release $Tag" -Action {
        Invoke-RestMethod -UseBasicParsing -Uri $url -Headers $headers
    }
    if ($response -is [string]) {
        return $response | ConvertFrom-Json
    }
    return $response
}

function Get-ReleaseAsset {
    param(
        [Parameter(Mandatory = $true)] $Release,
        [Parameter(Mandatory = $true)][string] $AssetName,
        [Parameter(Mandatory = $true)][string] $ReleaseTag
    )

    $assetMatches = @($Release.assets | Where-Object { [string]$_.name -eq $AssetName })
    if ($assetMatches.Count -ne 1) {
        throw "Expected one release asset named $AssetName, found $($assetMatches.Count)."
    }

    $digest = [string]$assetMatches[0].digest
    if ($digest -notmatch "^sha256:([0-9a-fA-F]{64})$") {
        throw "GitHub did not provide a SHA-256 digest for $AssetName."
    }

    return [pscustomobject]@{
        Name = $AssetName
        Url = "$($GitHubDownloadBaseUrl.TrimEnd('/'))/$Repository/releases/download/$([Uri]::EscapeDataString($ReleaseTag))/$([Uri]::EscapeDataString($AssetName))"
        Sha256 = $Matches[1].ToLowerInvariant()
        Digest = $digest.ToLowerInvariant()
        Size = [int64]$assetMatches[0].size
    }
}

function Resolve-ReleaseCommit {
    param(
        [Parameter(Mandatory = $true)] $Release,
        [Parameter(Mandatory = $true)][string] $Tag
    )

    $target = [string]$Release.target_commitish
    if ($target -match "^[0-9a-fA-F]{40}$") {
        return $target.ToLowerInvariant()
    }

    $encodedTag = [Uri]::EscapeDataString($Tag)
    $referenceUrl = "$($GitHubApiBaseUrl.TrimEnd('/'))/repos/$Repository/git/ref/tags/$encodedTag"
    $reference = Invoke-WithRetry -Description "Resolving GitHub tag $Tag" -Action {
        Invoke-RestMethod -UseBasicParsing -Uri $referenceUrl -Headers $headers
    }
    if ([string]$reference.object.type -eq "commit") {
        return ([string]$reference.object.sha).ToLowerInvariant()
    }
    if ([string]$reference.object.type -eq "tag") {
        $tagUrl = "$($GitHubApiBaseUrl.TrimEnd('/'))/repos/$Repository/git/tags/$($reference.object.sha)"
        $tagObject = Invoke-WithRetry -Description "Resolving annotated GitHub tag $Tag" -Action {
            Invoke-RestMethod -UseBasicParsing -Uri $tagUrl -Headers $headers
        }
        if ([string]$tagObject.object.type -eq "commit") {
            return ([string]$tagObject.object.sha).ToLowerInvariant()
        }
    }

    throw "Could not resolve release tag $Tag to a Git commit."
}

function Get-InstalledClientCommit {
    param([Parameter(Mandatory = $true)][string] $Root)

    $ciIdPath = Join-Path $Root "CI-ID.txt"
    if (-not (Test-Path -LiteralPath $ciIdPath -PathType Leaf)) {
        return $null
    }
    foreach ($line in Get-Content -LiteralPath $ciIdPath) {
        if ($line -match "^Commit\s+([0-9a-fA-F]{40})\s*$") {
            return $Matches[1].ToLowerInvariant()
        }
    }
    return $null
}

function Get-InstalledClientChannel {
    param([Parameter(Mandatory = $true)][string] $Root)

    $path = Join-Path $Root "fetcher-client-channel.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return ""
    }
    try {
        $channel = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
        return ([string]$channel.channel).ToLowerInvariant()
    }
    catch {
        return ""
    }
}

function Assert-OpenMwStopped {
    param([Parameter(Mandatory = $true)][string] $Root)

    $normalizedRoot = [IO.Path]::GetFullPath($Root).TrimEnd("\") + "\"
    $running = New-Object System.Collections.Generic.List[string]
    foreach ($processName in @("openmw", "openmw-launcher", "openmw-cs", "openmw-server")) {
        foreach ($process in Get-Process -Name $processName -ErrorAction SilentlyContinue) {
            try {
                $processPath = [IO.Path]::GetFullPath([string]$process.Path)
                if ($processPath.StartsWith($normalizedRoot, [StringComparison]::OrdinalIgnoreCase)) {
                    $running.Add("$($process.ProcessName) (PID $($process.Id))")
                }
            }
            catch {
                # Processes outside this portable install do not block its update.
            }
        }
    }
    if ($running.Count -gt 0) {
        throw "Close Fetcher Simulator before updating: $($running -join ', ')."
    }
}

function Read-ClientInventory {
    param([Parameter(Mandatory = $true)][string] $Path)

    $inventory = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ([int]$inventory.schemaVersion -ne 1) {
        throw "Unsupported Fetcher client inventory schema: $($inventory.schemaVersion)"
    }
    return $inventory
}

function Install-ClientArchive {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)] $Asset,
        [Parameter(Mandatory = $true)][string] $RemoteCommit,
        [Parameter(Mandatory = $true)][string] $RunWorkRoot
    )

    Assert-OpenMwStopped -Root $Root
    $archivePath = Join-Path $RunWorkRoot $Asset.Name
    $extractRoot = Join-Path $RunWorkRoot "client"
    Write-Host "Downloading Fetcher Simulator client update..."
    Write-Host "  $($Asset.Url)"
    Invoke-WithRetry -Description "Downloading $($Asset.Name)" -Action {
        Invoke-WebRequest -UseBasicParsing -Uri $Asset.Url -Headers $headers -OutFile $archivePath
    } | Out-Null

    $archiveHash = Get-Sha256 -Path $archivePath
    if ($archiveHash -ne $Asset.Sha256) {
        throw "Client archive checksum mismatch. Expected $($Asset.Sha256), got $archiveHash."
    }
    Assert-SafeArchivePaths -ArchivePath $archivePath
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot

    $newInventoryPath = Join-Path $extractRoot "fetcher-client-files.json"
    if (-not (Test-Path -LiteralPath $newInventoryPath -PathType Leaf)) {
        throw "The client archive does not contain fetcher-client-files.json."
    }
    $newInventory = Read-ClientInventory -Path $newInventoryPath
    if ([string]$newInventory.clientCommit -ne $RemoteCommit) {
        throw "Client inventory commit $($newInventory.clientCommit) does not match release commit $RemoteCommit."
    }

    $newFiles = @{}
    foreach ($record in @($newInventory.files)) {
        $relativePath = ConvertTo-NormalizedRelativePath -RelativePath ([string]$record.path)
        if (Test-FetcherMutablePath -RelativePath $relativePath) {
            throw "Client inventory attempts to manage protected path: $relativePath"
        }
        if ($newFiles.ContainsKey($relativePath)) {
            throw "Client inventory contains duplicate path: $relativePath"
        }
        $sourcePath = Join-Path $extractRoot $relativePath.Replace("/", "\")
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Client archive is missing managed file: $relativePath"
        }
        if ((Get-Item -LiteralPath $sourcePath).Length -ne [int64]$record.size -or
            (Get-Sha256 -Path $sourcePath) -ne ([string]$record.sha256).ToLowerInvariant()) {
            throw "Client archive file failed inventory verification: $relativePath"
        }
        $newFiles[$relativePath] = $record
    }

    $oldFiles = @{}
    $installedInventoryPath = Join-Path $Root "fetcher-client-files.json"
    if (Test-Path -LiteralPath $installedInventoryPath -PathType Leaf) {
        $oldInventory = Read-ClientInventory -Path $installedInventoryPath
        foreach ($record in @($oldInventory.files)) {
            $relativePath = ConvertTo-NormalizedRelativePath -RelativePath ([string]$record.path)
            if (-not (Test-FetcherMutablePath -RelativePath $relativePath)) {
                $oldFiles[$relativePath] = $record
            }
        }
    }

    $rollbackRoot = Join-Path $RunWorkRoot "rollback"
    $changes = New-Object System.Collections.Generic.List[object]
    try {
        foreach ($relativePath in @($newFiles.Keys | Sort-Object)) {
            $record = $newFiles[$relativePath]
            $sourcePath = Join-Path $extractRoot $relativePath.Replace("/", "\")
            $destinationPath = Join-Path $Root $relativePath.Replace("/", "\")
            if ((Test-Path -LiteralPath $destinationPath -PathType Leaf) -and
                (Get-Sha256 -Path $destinationPath) -eq ([string]$record.sha256).ToLowerInvariant()) {
                continue
            }

            $existed = Test-Path -LiteralPath $destinationPath -PathType Leaf
            $backupPath = Join-Path $rollbackRoot $relativePath.Replace("/", "\")
            if ($existed) {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backupPath) | Out-Null
                Copy-Item -LiteralPath $destinationPath -Destination $backupPath -Force
            }
            $changes.Add([pscustomobject]@{ Destination = $destinationPath; Backup = $backupPath; Existed = $existed })
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationPath) | Out-Null
            Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
            if ((Get-Sha256 -Path $destinationPath) -ne ([string]$record.sha256).ToLowerInvariant()) {
                throw "Installed client file failed verification: $relativePath"
            }
        }

        foreach ($relativePath in @($oldFiles.Keys | Where-Object { -not $newFiles.ContainsKey($_) } | Sort-Object)) {
            if (Test-FetcherMutablePath -RelativePath $relativePath) {
                continue
            }
            $destinationPath = Join-Path $Root $relativePath.Replace("/", "\")
            if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
                continue
            }
            $backupPath = Join-Path $rollbackRoot $relativePath.Replace("/", "\")
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backupPath) | Out-Null
            Copy-Item -LiteralPath $destinationPath -Destination $backupPath -Force
            $changes.Add([pscustomobject]@{ Destination = $destinationPath; Backup = $backupPath; Existed = $true })
            Remove-Item -LiteralPath $destinationPath -Force
        }

        $inventoryBackup = Join-Path $rollbackRoot "fetcher-client-files.json"
        $inventoryExisted = Test-Path -LiteralPath $installedInventoryPath -PathType Leaf
        if ($inventoryExisted) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $inventoryBackup) | Out-Null
            Copy-Item -LiteralPath $installedInventoryPath -Destination $inventoryBackup -Force
        }
        $changes.Add([pscustomobject]@{ Destination = $installedInventoryPath; Backup = $inventoryBackup; Existed = $inventoryExisted })
        Copy-Item -LiteralPath $newInventoryPath -Destination $installedInventoryPath -Force
    }
    catch {
        for ($index = $changes.Count - 1; $index -ge 0; --$index) {
            $change = $changes[$index]
            if ($change.Existed -and (Test-Path -LiteralPath $change.Backup -PathType Leaf)) {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $change.Destination) | Out-Null
                Copy-Item -LiteralPath $change.Backup -Destination $change.Destination -Force
            }
            elseif (-not $change.Existed -and (Test-Path -LiteralPath $change.Destination -PathType Leaf)) {
                Remove-Item -LiteralPath $change.Destination -Force
            }
        }
        throw
    }

    Write-Host "Fetcher Simulator client updated to commit $RemoteCommit."
}

function Resolve-ConfiguredDataPath {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $Value
    )

    $path = $Value.Trim()
    if ($path.Length -ge 2 -and $path.StartsWith('"') -and $path.EndsWith('"')) {
        $path = $path.Substring(1, $path.Length - 2)
    }
    if (-not [IO.Path]::IsPathRooted($path)) {
        $path = Join-Path $Root $path
    }
    if (Test-Path -LiteralPath $path -PathType Container) {
        return (Resolve-Path -LiteralPath $path).Path
    }
    return $null
}

function Find-OpenMwPluginDataRoot {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $Plugin,
        [Parameter(Mandatory = $true)][string] $RequiredSubdirectory
    )

    $candidates = New-Object System.Collections.Generic.List[string]
    $seen = New-Object System.Collections.Generic.HashSet[string]([StringComparer]::OrdinalIgnoreCase)
    $managedRoot = [IO.Path]::GetFullPath((Join-Path $Root "Data Files\fetcher-bardcraft")).TrimEnd("\", "/") + "\"

    function Select-ManagedCandidate {
        param([Parameter(Mandatory = $true)] $Paths)
        $managed = @($Paths | Where-Object {
            [IO.Path]::GetFullPath([string]$_).StartsWith($managedRoot, [StringComparison]::OrdinalIgnoreCase)
        })
        if ($managed.Count -eq 1) {
            return $managed[0]
        }
        return $null
    }

    $configPath = Join-Path $Root "openmw.cfg"
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $configPath) {
            if ($line -notmatch "^\s*data\s*=\s*(.+?)\s*$") {
                continue
            }
            $dataPath = Resolve-ConfiguredDataPath -Root $Root -Value $Matches[1]
            if ($null -ne $dataPath -and
                (Test-Path -LiteralPath (Join-Path $dataPath $Plugin) -PathType Leaf) -and
                (Test-Path -LiteralPath (Join-Path $dataPath $RequiredSubdirectory) -PathType Container) -and
                $seen.Add($dataPath)) {
                $candidates.Add($dataPath)
            }
        }
    }

    if ($candidates.Count -eq 1) {
        return $candidates[0]
    }
    if ($candidates.Count -gt 1) {
        $managedCandidate = Select-ManagedCandidate -Paths $candidates
        if ($null -ne $managedCandidate) {
            Write-Host "Using updater-managed $Plugin installation:"
            Write-Host "  $managedCandidate"
            return $managedCandidate
        }
        throw "Multiple active data paths contain $Plugin. Remove duplicate data= entries before updating."
    }

    $dataFilesRoot = Join-Path $Root "Data Files"
    if (Test-Path -LiteralPath $dataFilesRoot -PathType Container) {
        foreach ($pluginFile in Get-ChildItem -LiteralPath $dataFilesRoot -Recurse -Force -File -Filter $Plugin -ErrorAction SilentlyContinue) {
            $dataPath = $pluginFile.Directory.FullName
            if ((Test-Path -LiteralPath (Join-Path $dataPath $RequiredSubdirectory) -PathType Container) -and $seen.Add($dataPath)) {
                $candidates.Add($dataPath)
            }
        }
    }

    if ($candidates.Count -eq 1) {
        return $candidates[0]
    }
    if ($candidates.Count -gt 1) {
        $managedCandidate = Select-ManagedCandidate -Paths $candidates
        if ($null -ne $managedCandidate) {
            return $managedCandidate
        }
        throw "Found multiple installations of $Plugin. Add the intended folder to openmw.cfg and remove stale duplicate data= entries."
    }
    return $null
}

function Install-ClientModPatch {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)] $Patch,
        [Parameter(Mandatory = $true)][string] $UpdateRoot,
        [Parameter(Mandatory = $true)][string] $RunWorkRoot,
        [Parameter(Mandatory = $true)][hashtable] $PatchStates
    )

    $targetRoot = Find-OpenMwPluginDataRoot -Root $Root -Plugin ([string]$Patch.targetPlugin) `
        -RequiredSubdirectory ([string]$Patch.requiredSubdirectory)
    if ($null -eq $targetRoot) {
        Write-Warning "$($Patch.name) was not applied because $($Patch.targetPlugin) is not installed."
        return
    }

    $release = Get-GitHubRelease -Tag ([string]$Patch.releaseTag)
    $asset = Get-ReleaseAsset -Release $release -AssetName ([string]$Patch.assetName) `
        -ReleaseTag ([string]$Patch.releaseTag)
    $cacheRoot = Join-Path $UpdateRoot ("patches\{0}" -f [string]$Patch.id)
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
    $archivePath = Join-Path $cacheRoot ("{0}-{1}.zip" -f [string]$Patch.id, $asset.Sha256)
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf) -or (Get-Sha256 -Path $archivePath) -ne $asset.Sha256) {
        Write-Host "Downloading $($Patch.name)..."
        Write-Host "  $($asset.Url)"
        Invoke-WithRetry -Description "Downloading $($Patch.assetName)" -Action {
            Invoke-WebRequest -UseBasicParsing -Uri $asset.Url -Headers $headers -OutFile $archivePath
        } | Out-Null
    }
    if ((Get-Sha256 -Path $archivePath) -ne $asset.Sha256) {
        throw "$($Patch.name) archive checksum verification failed."
    }

    Assert-SafeArchivePaths -ArchivePath $archivePath
    $extractRoot = Join-Path $RunWorkRoot ("patch-{0}" -f [string]$Patch.id)
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot
    $appliers = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File -Filter ([string]$Patch.applierPattern))
    if ($appliers.Count -ne 1) {
        throw "Expected one $($Patch.applierPattern) in $($Patch.assetName), found $($appliers.Count)."
    }

    $manifestName = "fetcher-bardcraft-mp-patch.json"
    $manifestNameProperty = $Patch.PSObject.Properties["manifestName"]
    if ($null -ne $manifestNameProperty -and -not [string]::IsNullOrWhiteSpace([string]$manifestNameProperty.Value)) {
        $manifestName = [string]$manifestNameProperty.Value
    }
    $manifestPath = Join-Path $extractRoot $manifestName
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "$($Patch.assetName) does not contain its patch manifest."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $invokeParameters = @{}
    $invokeParameters[[string]$Patch.targetParameter] = $targetRoot
    $installRootProperty = $Patch.PSObject.Properties["installRootParameter"]
    if ($null -ne $installRootProperty -and
        -not [string]::IsNullOrWhiteSpace([string]$installRootProperty.Value)) {
        $invokeParameters[[string]$installRootProperty.Value] = $Root
    }
    Write-Host "Applying $($Patch.name) to:"
    Write-Host "  $targetRoot"
    & $appliers[0].FullName @invokeParameters

    $PatchStates[[string]$Patch.id] = [ordered]@{
        assetDigest = $asset.Digest
        patchVersion = [string]$manifest.patchVersion
        manifestSha256 = Get-Sha256 -Path $manifestPath
        target = $targetRoot
        updatedAtUtc = [DateTime]::UtcNow.ToString("o")
    }
}

function Install-UmoModList {
    param([Parameter(Mandatory = $true)][string] $Root)

    $installer = $UmoInstallerPath
    if ([string]::IsNullOrWhiteSpace($installer)) {
        $installer = Join-Path $Root "Install-Fetcher-Bardcraft-With-UMO.ps1"
    }
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        throw "UMO mod installer was not found: $installer"
    }

    Assert-OpenMwStopped -Root $Root
    Write-Host "Checking required Fetcher mods and dependencies through UMO..."
    & $installer `
        -UmoBasePath (Join-Path $Root "Data Files") `
        -ApplyBardcraftMultiplayerPatch $false `
        -ApplyPublicTestConfig $true
    if (-not $?) {
        throw "Fetcher UMO mod installation failed."
    }
}

function Install-TesterTools {
    param([Parameter(Mandatory = $true)][string] $Root)

    $bootstrap = Join-Path $Root "Install-Fetcher-Tester-Tools.ps1"
    if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
        throw "Fetcher tester tools bootstrap was not found: $bootstrap"
    }

    Write-Host "Refreshing Fetcher tester tools..."
    $parameters = @{
        InstallRoot = $Root
        Repository = $Repository
        ReleaseTag = $TesterToolsReleaseTag
        AssetName = $TesterToolsAssetName
        GitHubApiBaseUrl = $GitHubApiBaseUrl
        GitHubDownloadBaseUrl = $GitHubDownloadBaseUrl
        SkipUpdater = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($TesterToolsArchivePath)) {
        $parameters.ToolsArchivePath = $TesterToolsArchivePath
    }
    & $bootstrap @parameters
    if (-not $?) {
        throw "Fetcher tester tools refresh failed."
    }
}

function Write-UpdateState {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)] $ClientState,
        [Parameter(Mandatory = $true)][hashtable] $PatchStates
    )

    $orderedPatches = [ordered]@{}
    foreach ($key in @($PatchStates.Keys | Sort-Object)) {
        $orderedPatches[$key] = $PatchStates[$key]
    }
    $state = [ordered]@{
        schemaVersion = 1
        client = $ClientState
        patches = $orderedPatches
        checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    }
    $temporaryPath = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporaryPath -Encoding UTF8
    Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
}

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$root = (Resolve-Path -LiteralPath $InstallRoot).Path.TrimEnd("\", "/")
$rootHashAlgorithm = [Security.Cryptography.SHA256]::Create()
try {
    $rootHashBytes = $rootHashAlgorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))
}
finally {
    $rootHashAlgorithm.Dispose()
}
$rootHash = ([BitConverter]::ToString($rootHashBytes)).Replace("-", "").ToLowerInvariant()
$updateMutex = New-Object Threading.Mutex($false, "Local\FetcherSimulatorUpdater-$rootHash")
$mutexAcquired = $false
try {
    try {
        $mutexAcquired = $updateMutex.WaitOne(0)
    }
    catch [Threading.AbandonedMutexException] {
        $mutexAcquired = $true
    }
    if (-not $mutexAcquired) {
        throw "Another Fetcher Simulator updater is already running for $root."
    }

$updateRoot = Join-Path $root "_fetcher_update"
$workParent = Join-Path $updateRoot "work"
if (Test-Path -LiteralPath $workParent -PathType Container) {
    foreach ($staleWork in Get-ChildItem -LiteralPath $workParent -Force -Directory) {
        Remove-Item -LiteralPath $staleWork.FullName -Recurse -Force
    }
}
$runWorkRoot = Join-Path $workParent ([Guid]::NewGuid().ToString("N"))
$statePath = Join-Path $root "fetcher-update-state.json"
New-Item -ItemType Directory -Force -Path $runWorkRoot | Out-Null

$clientState = [ordered]@{}
$patchStates = @{}
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
    try {
        $previousState = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if ($null -ne $previousState.client) {
            foreach ($property in $previousState.client.PSObject.Properties) {
                $clientState[$property.Name] = $property.Value
            }
        }
        if ($null -ne $previousState.patches) {
            foreach ($property in $previousState.patches.PSObject.Properties) {
                $patchStates[$property.Name] = $property.Value
            }
        }
    }
    catch {
        Write-Warning "Ignoring unreadable updater state: $($_.Exception.Message)"
    }
}

try {
    if (-not $SkipClientUpdate) {
        Write-Host "Checking Fetcher Simulator client release..."
        $clientRelease = Get-GitHubRelease -Tag $ClientReleaseTag
        $clientAsset = Get-ReleaseAsset -Release $clientRelease -AssetName $ClientAssetName -ReleaseTag $ClientReleaseTag
        $remoteCommit = Resolve-ReleaseCommit -Release $clientRelease -Tag $ClientReleaseTag
        $localCommit = Get-InstalledClientCommit -Root $root
        $localChannel = Get-InstalledClientChannel -Root $root
        $knownAssetDigest = if ($clientState.Contains("assetDigest")) { [string]$clientState["assetDigest"] } else { "" }
        $knownReleaseTag = if ($clientState.Contains("releaseTag")) { [string]$clientState["releaseTag"] } else { "" }
        if ($localChannel -ne "test" -or $localCommit -ne $remoteCommit -or $knownReleaseTag -ne $ClientReleaseTag -or
            (-not [string]::IsNullOrWhiteSpace($knownAssetDigest) -and $knownAssetDigest -ne $clientAsset.Digest)) {
            Install-ClientArchive -Root $root -Asset $clientAsset -RemoteCommit $remoteCommit -RunWorkRoot $runWorkRoot
        }
        else {
            Write-Host "Client is current at commit $remoteCommit."
        }
        $clientState = [ordered]@{
            commit = $remoteCommit
            releaseTag = $ClientReleaseTag
            assetName = $ClientAssetName
            assetDigest = $clientAsset.Digest
            checkedAtUtc = [DateTime]::UtcNow.ToString("o")
        }
    }

    if (-not $SkipTesterToolsUpdate) {
        Install-TesterTools -Root $root
    }

    if (-not $SkipUmoMods) {
        Install-UmoModList -Root $root
    }

    $zhiCompatibilityScript = Join-Path $root "Apply-Fetcher-ZHI-Compatibility.ps1"
    if (Test-Path -LiteralPath $zhiCompatibilityScript -PathType Leaf) {
        & $zhiCompatibilityScript -InstallRoot $root
        if (-not $?) {
            throw "Zerkish Hotkeys Improved compatibility fix failed."
        }
    }

    if (-not $SkipModPatches) {
        if ([string]::IsNullOrWhiteSpace($PatchCatalogPath)) {
            $PatchCatalogPath = Join-Path $root "fetcher-client-patches.json"
        }
        if (Test-Path -LiteralPath $PatchCatalogPath -PathType Leaf) {
            $catalog = Get-Content -LiteralPath $PatchCatalogPath -Raw | ConvertFrom-Json
            if ([int]$catalog.schemaVersion -ne 1) {
                throw "Unsupported Fetcher client patch catalog schema: $($catalog.schemaVersion)"
            }
            foreach ($patch in @($catalog.patches)) {
                Install-ClientModPatch -Root $root -Patch $patch -UpdateRoot $updateRoot `
                    -RunWorkRoot $runWorkRoot -PatchStates $patchStates
            }
        }
        else {
            Write-Warning "Client patch catalog was not found: $PatchCatalogPath"
        }

        $publicConfigScript = Join-Path $root "Apply-Fetcher-Public-Test-Config.ps1"
        if (-not (Test-Path -LiteralPath $publicConfigScript -PathType Leaf)) {
            throw "Public test configuration script was not found: $publicConfigScript"
        }
        Write-Host "Regenerating openmw.cfg after compatibility patches..."
        & $publicConfigScript
        if (-not $?) {
            throw "Fetcher public test configuration regeneration failed."
        }
    }

    Write-UpdateState -Path $statePath -ClientState $clientState -PatchStates $patchStates
    Write-Host "Update check completed successfully."
}
finally {
    if (Test-Path -LiteralPath $runWorkRoot -PathType Container) {
        Remove-Item -LiteralPath $runWorkRoot -Recurse -Force
    }
}
}
finally {
    if ($mutexAcquired) {
        [void]$updateMutex.ReleaseMutex()
    }
    $updateMutex.Dispose()
}
