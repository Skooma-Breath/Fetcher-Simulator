[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $CompatibilityBuildRoot,
    [Parameter(Mandatory = $true)][string] $OutputDirectory,
    [string] $PatchVersion = "2.1.0",
    [string] $SourceCommit = ""
)

$ErrorActionPreference = "Stop"
$sourceRoot = (Resolve-Path -LiteralPath $CompatibilityBuildRoot).Path
$sourceDataFiles = Join-Path $sourceRoot "Data Files"
$sourceOverlay = Join-Path $sourceRoot "Starwind Vanilla Compat"
$musicRouterSource = Join-Path $PSScriptRoot "StarwindMusicRouter.lua"
$musicCellsSource = Join-Path $PSScriptRoot "StarwindMusicCells.lua"
foreach ($required in @(
    (Join-Path $sourceDataFiles "StarwindRemasteredV1.15.esm"),
    (Join-Path $sourceDataFiles "StarwindRemasteredPatch.esm"),
    $sourceOverlay,
    $musicRouterSource,
    $musicCellsSource
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required compatibility build output is missing: $required"
    }
}
$requiredCreatureAssets = [ordered]@{
    "Meshes\starwind_compat\r\xancestorghost.kf" = "ae083bacc62358e673ad73043fd05658a52b8a47c924fe3051e960fef4c56511"
    "Meshes\starwind_compat\r\xashslave.kf" = "b0c2782b91fe6492a68da6afe6a473c583a6b6bb5cf9d89bb42e9d1c9618012c"
    "Meshes\starwind_compat\r\xbyagram.kf" = "18054c50dd91b31351bd9ed6ac3fa3c55c593327c69c713a8a663f9d82820cf8"
    "Meshes\starwind_compat\r\xcavemudcrab.kf" = "86b05b1e08b24be6c4ec74eeec3e2720505bb1ee5b5147cb1e4c01968e43c264"
    "Meshes\starwind_compat\r\xcliffracer.kf" = "a6abc12f648a2b1c1209bc11aa8e789336f741a06f16a8c026a9e2656b5bd078"
    "Meshes\starwind_compat\r\xdurzog.kf" = "bdcc26072f96a219e966fe0669589e538b10ede71293e12960978733ae79d946"
    "Meshes\starwind_compat\r\xduskyalit.kf" = "1fe15ae449e6891edc02ba1661a61f4ceaa51cb02648635d3674f6f677d44ca5"
    "Meshes\starwind_compat\r\xfabricant.kf" = "ef07c46df988d85d9fb32d3103cafee1fb2d216c95606e7b0a2cdd1e90b73f82"
    "Meshes\starwind_compat\r\xfabricant_hulking.kf" = "6cc2868ddf6820f7d32b415a4428cb71d500c1b1f2808627bcd9c971705f4e58"
    "Meshes\starwind_compat\r\xfabricant_imperfect.kf" = "6819a5400993c4890bd7d32b3ab1b32232a8228eddf36a8d4a733fc5938bf00f"
    "Meshes\starwind_compat\r\xfabricant_imperfect.nif" = "d99f8362661af5544356bf86d7c76627fdcb6d9599fbd06865d6aaf56b1962f5"
    "Meshes\starwind_compat\r\xfrostgiant.kf" = "f11ab25748b745a7df38a8182bbd0360424cc574d1e20b98644254df8c0f0450"
    "Meshes\starwind_compat\r\xgreatbonewalker.kf" = "f25018d2aa2b2ed4c9440657a3bdd586cdd5425a05c9157bff3486fdd480bb3f"
    "Meshes\starwind_compat\r\xguar.kf" = "2345b650938fff241079fc7af2252fd00fbb4f87e20b20b2fe3519d1ae682c5a"
    "Meshes\starwind_compat\r\xice troll.kf" = "e677b4ba1475ca6af850ca90af3e4620472f9d3ff1fe0f094a3240b8b729c6ce"
    "Meshes\starwind_compat\r\xkwama forager.kf" = "ba193c9b20f0713400ef45aafe1b046d480065ab91a8c45f909abb07e843e834"
    "Meshes\starwind_compat\r\xkwama warior.kf" = "4a9de7f1a1c24557ea0c8dddfb146bc6623d150c683b29b2ee34b36d44a5161e"
    "Meshes\starwind_compat\r\xleastkagouti.kf" = "f16948a9dd981ac1f051989dcd008e22cb63f84d00d23e678eb88337cd178c76"
    "Meshes\starwind_compat\r\xlordvivec.kf" = "6d9108a52b09ff6b834e2925ce8786b7f285f95d7ba5c04f4a157c0a26f9fef7"
    "Meshes\starwind_compat\r\xminescrib.kf" = "7e95aafcf1fac5cafdfccf0a949897758499ec9baaa6f198cc4107658afe28a3"
    "Meshes\starwind_compat\r\xnixhound.kf" = "86bbd5a4d9b3c70d6ad6af63dc7517cc9357cac6b25b15f9a327628f89e85402"
    "Meshes\starwind_compat\r\xscamp_fetch.kf" = "2c20c59c33faf0662727a6e20874482029bf21be8bf769bf64779b1f2791657a"
}
foreach ($entry in $requiredCreatureAssets.GetEnumerator()) {
    $assetPath = Join-Path $sourceOverlay $entry.Key
    if (-not (Test-Path -LiteralPath $assetPath -PathType Leaf)) {
        throw "Required relocated creature companion is missing: $assetPath"
    }
    $assetHash = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($assetHash -ne $entry.Value) {
        throw "Relocated creature companion checksum mismatch for $($entry.Key). Expected $($entry.Value) but got $assetHash."
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
    $payloadOverlay = Join-Path $payloadRoot "Starwind Vanilla Compat"
    New-Item -ItemType Directory -Force -Path $payloadOverlay | Out-Null
    foreach ($overlayItem in Get-ChildItem -LiteralPath $sourceOverlay -Force) {
        if ($overlayItem.Name -ieq "Music") {
            continue
        }
        Copy-Item -LiteralPath $overlayItem.FullName -Destination $payloadOverlay -Recurse -Force
    }
    $payloadScripts = Join-Path $payloadOverlay "scripts\starwind-compat"
    New-Item -ItemType Directory -Force -Path $payloadScripts | Out-Null
    Copy-Item -LiteralPath $musicRouterSource -Destination (Join-Path $payloadScripts "starwind-music-router.lua") -Force
    Copy-Item -LiteralPath $musicCellsSource -Destination (Join-Path $payloadScripts "starwind-music-cells.lua") -Force

    $descriptorPath = Join-Path $payloadOverlay "StarwindVanillaCompat.omwscripts"
    if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) {
        throw "StarwindVanillaCompat.omwscripts is missing from the compatibility overlay."
    }
    $musicScriptRegistration = "PLAYER: scripts/starwind-compat/starwind-music-router.lua"
    if (-not (Get-Content -LiteralPath $descriptorPath | Where-Object { $_.Trim() -eq $musicScriptRegistration })) {
        [IO.File]::AppendAllText(
            $descriptorPath,
            [Environment]::NewLine + $musicScriptRegistration + [Environment]::NewLine,
            [Text.UTF8Encoding]::new($false)
        )
    }
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
