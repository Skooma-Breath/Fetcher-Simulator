[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Repository,

    [Parameter(Mandatory)]
    [string] $Tag,

    [Parameter(Mandatory)]
    [string] $TargetCommit,

    [Parameter(Mandatory)]
    [string] $Title,

    [Parameter(Mandatory)]
    [string] $Notes,

    [Parameter(Mandatory)]
    [string[]] $Assets
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($env:GH_TOKEN)) {
    throw "GH_TOKEN is required to publish a GitHub release."
}

$assetPaths = @($Assets | ForEach-Object {
    $resolved = Resolve-Path -LiteralPath $_ -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolved.Path -PathType Leaf)) {
        throw "Release asset was not found: $_"
    }
    $resolved.Path
})

$publicHeaders = @{
    Accept = "application/vnd.github+json"
    "User-Agent" = "Fetcher-Simulator-Actions"
    "X-GitHub-Api-Version" = "2022-11-28"
}
$writeHeaders = @{
    Accept = "application/vnd.github+json"
    Authorization = "Bearer $env:GH_TOKEN"
    "User-Agent" = "Fetcher-Simulator-Actions"
    "X-GitHub-Api-Version" = "2022-11-28"
}

$escapedTag = [Uri]::EscapeDataString($Tag)
$releaseUri = "https://api.github.com/repos/$Repository/releases/tags/$escapedTag"
$release = $null

try {
    # This repository is public, so discovery does not depend on workflow-token
    # permissions or gh CLI release/tag resolution.
    $release = Invoke-RestMethod -Method Get -Uri $releaseUri -Headers $publicHeaders
}
catch {
    $statusCode = if ($null -ne $_.Exception.Response) {
        [int] $_.Exception.Response.StatusCode
    }
    else {
        0
    }
    if ($statusCode -ne 404) {
        throw
    }
}

if ($null -eq $release) {
    $createBody = @{
        tag_name = $Tag
        target_commitish = $TargetCommit
        name = $Title
        body = $Notes
        draft = $false
        prerelease = $true
    } | ConvertTo-Json

    $release = Invoke-RestMethod `
        -Method Post `
        -Uri "https://api.github.com/repos/$Repository/releases" `
        -Headers $writeHeaders `
        -ContentType "application/json" `
        -Body $createBody
}

foreach ($assetPath in $assetPaths) {
    $assetName = Split-Path -Leaf $assetPath
    foreach ($existingAsset in @($release.assets | Where-Object name -eq $assetName)) {
        Invoke-RestMethod `
            -Method Delete `
            -Uri "https://api.github.com/repos/$Repository/releases/assets/$($existingAsset.id)" `
            -Headers $writeHeaders | Out-Null
    }

    $encodedName = [Uri]::EscapeDataString($assetName)
    Invoke-RestMethod `
        -Method Post `
        -Uri "https://uploads.github.com/repos/$Repository/releases/$($release.id)/assets?name=$encodedName" `
        -Headers $writeHeaders `
        -ContentType "application/octet-stream" `
        -InFile $assetPath | Out-Null

    Write-Host "Published release asset: $assetName"
}

# Keep the stable tag aligned with the package source only after every release
# asset has been replaced successfully.
git tag -f $Tag $TargetCommit
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create local tag $Tag."
}

git push origin "refs/tags/$Tag" --force
if ($LASTEXITCODE -ne 0) {
    throw "Failed to update tag $Tag."
}
