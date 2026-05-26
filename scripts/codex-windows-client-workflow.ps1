param(
    [string]$Repo = "Skooma-Breath/Fetcher-Simulator",
    [string]$Workflow = "codex-windows-client.yml",
    [string]$Ref = "main",
    [ValidateSet("2022")]
    [string]$Image = "2022",
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string]$BuildType = "RelWithDebInfo",
    [bool]$Package = $false,
    [bool]$Release = $false,
    [string]$ReleaseTag = "",
    [string]$ReleaseName = "",
    [string]$ModBundleUrl = "",
    [string]$ModBundleReleaseTag = "",
    [string]$ModBundleAsset = "openmw-client-mods.zip",
    [string]$RunLabel = "codex",
    [string]$OutDir = "remote-workflow-logs"
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI 'gh' was not found on PATH. Install it and run 'gh auth login' first."
}

function Invoke-GhChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "gh $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

function Invoke-GhJson {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "gh $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }

    if (-not $output) {
        return @()
    }

    return @($output | ConvertFrom-Json)
}

& gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    throw "GitHub CLI is not authenticated. Run 'gh auth login' first."
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$packageInput = $Package.ToString().ToLowerInvariant()
$releaseInput = $Release.ToString().ToLowerInvariant()

$existingRuns = Invoke-GhJson @(
    "run", "list",
    "--repo", $Repo,
    "--workflow", $Workflow,
    "--branch", $Ref,
    "--limit", "20",
    "--json", "databaseId"
)
$existingRunIds = @{}
foreach ($run in $existingRuns) {
    $existingRunIds[[string]$run.databaseId] = $true
}

Write-Host "Triggering $Workflow on $Repo@$Ref..."
$workflowRunArgs = @(
    "workflow", "run", $Workflow,
    "--repo", $Repo,
    "--ref", $Ref,
    "-f", "image=$Image",
    "-f", "build-type=$BuildType",
    "-f", "package=$packageInput",
    "-f", "release=$releaseInput",
    "-f", "run-label=$RunLabel"
)
if ($ReleaseTag) {
    $workflowRunArgs += @("-f", "release-tag=$ReleaseTag")
}
if ($ReleaseName) {
    $workflowRunArgs += @("-f", "release-name=$ReleaseName")
}
if ($ModBundleUrl) {
    $workflowRunArgs += @("-f", "mod-bundle-url=$ModBundleUrl")
}
if ($ModBundleReleaseTag) {
    $workflowRunArgs += @("-f", "mod-bundle-release-tag=$ModBundleReleaseTag")
}
if ($ModBundleAsset -ne "openmw-client-mods.zip") {
    $workflowRunArgs += @("-f", "mod-bundle-asset=$ModBundleAsset")
}
Invoke-GhChecked $workflowRunArgs

$runId = $null
$deadline = (Get-Date).AddMinutes(2)
do {
    Start-Sleep -Seconds 5
    $runs = Invoke-GhJson @(
        "run", "list",
        "--repo", $Repo,
        "--workflow", $Workflow,
        "--branch", $Ref,
        "--limit", "20",
        "--json", "databaseId,createdAt"
    )
    $newRun = $runs |
        Where-Object { -not $existingRunIds.ContainsKey([string]$_.databaseId) } |
        Sort-Object -Property createdAt -Descending |
        Select-Object -First 1
    if ($newRun) {
        $runId = [string]$newRun.databaseId
    }
} while (-not $runId -and (Get-Date) -lt $deadline)

if (-not $runId) {
    throw "Could not find the workflow run that was just triggered."
}

Write-Host "Watching run $runId..."
gh run watch $runId --repo $Repo --exit-status
$exitCode = $LASTEXITCODE

Write-Host "Writing raw GitHub job logs..."
$logPath = Join-Path $OutDir "github-run-$runId.log"
gh run view $runId --repo $Repo --log 2>&1 | Tee-Object -FilePath $logPath
$logExitCode = $LASTEXITCODE
if ($logExitCode -ne 0) {
    Write-Warning "Failed to write raw GitHub job logs for run $runId."
}

Write-Host "Downloading workflow artifacts, including ci-logs..."
$artifactDir = Join-Path $OutDir "artifacts-$runId"
gh run download $runId --repo $Repo --dir $artifactDir
$artifactExitCode = $LASTEXITCODE
if ($artifactExitCode -ne 0) {
    Write-Warning "Failed to download workflow artifacts for run $runId."
}

Write-Host "Run URL: https://github.com/$Repo/actions/runs/$runId"
Write-Host "Logs saved under: $OutDir"

if ($exitCode -ne 0) {
    exit $exitCode
}
if ($logExitCode -ne 0) {
    exit $logExitCode
}
if ($artifactExitCode -ne 0) {
    exit $artifactExitCode
}
