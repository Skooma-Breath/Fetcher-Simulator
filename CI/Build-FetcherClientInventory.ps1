param(
    [Parameter(Mandatory = $true)]
    [string] $InstallDir,
    [Parameter(Mandatory = $true)]
    [string] $ClientCommit
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function ConvertTo-NormalizedRelativePath {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $FullName
    )

    return $FullName.Substring($Root.Length).TrimStart("\", "/").Replace("\", "/")
}

function Test-FetcherMutablePath {
    param([Parameter(Mandatory = $true)][string] $RelativePath)

    $path = $RelativePath.Replace("\", "/").TrimStart("/").ToLowerInvariant()
    $exactPaths = @(
        "fetcher-client-files.json",
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
        "update-fetcher-simulator.bat"
    )
    if ($exactPaths -contains $path) {
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

$root = (Resolve-Path -LiteralPath $InstallDir).Path.TrimEnd("\", "/")
if ($ClientCommit -notmatch "^[0-9a-fA-F]{40}$") {
    throw "ClientCommit must be a full 40-character Git commit hash."
}

$records = New-Object System.Collections.Generic.List[object]
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -Force -File) {
    $relativePath = ConvertTo-NormalizedRelativePath -Root $root -FullName $file.FullName
    if (Test-FetcherMutablePath -RelativePath $relativePath) {
        continue
    }

    $records.Add([ordered]@{
        path = $relativePath
        size = [int64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    })
}

$inventory = [ordered]@{
    schemaVersion = 1
    protectionPolicyVersion = 1
    clientCommit = $ClientCommit.ToLowerInvariant()
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    files = @($records | Sort-Object { $_.path })
}

$inventoryPath = Join-Path $root "fetcher-client-files.json"
$inventory | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $inventoryPath -Encoding UTF8
Write-Output "Fetcher client inventory written: $inventoryPath ($($records.Count) managed files)"
