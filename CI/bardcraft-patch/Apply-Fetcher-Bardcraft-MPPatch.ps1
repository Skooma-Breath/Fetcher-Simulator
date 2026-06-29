[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BardcraftDataRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function ConvertTo-SafeRelativePath {
    param([Parameter(Mandatory = $true)][string] $Path)

    $normalized = $Path.Replace("/", "\").TrimStart("\")
    if ([string]::IsNullOrWhiteSpace($normalized) -or [IO.Path]::IsPathRooted($normalized) -or $normalized.Contains(":")) {
        throw "Patch contains an invalid target path: $Path"
    }
    $segments = @($normalized.Split("\", [StringSplitOptions]::RemoveEmptyEntries))
    if ($segments.Count -eq 0 -or @($segments | Where-Object { $_ -eq "." -or $_ -eq ".." }).Count -gt 0) {
        throw "Patch target path escapes the Bardcraft data root: $Path"
    }
    return ($segments -join "\")
}

function Resolve-PatchRecordLocation {
    param(
        [Parameter(Mandatory = $true)] $Record,
        [Parameter(Mandatory = $true)][string] $DataRoot,
        [Parameter(Mandatory = $true)][string] $ScriptRoot
    )

    $relativePath = ConvertTo-SafeRelativePath -Path ([string]$Record.path)
    $properties = @($Record.PSObject.Properties.Name)
    $targetBase = if ($properties -contains "targetBase") { [string]$Record.targetBase } else { "scripts" }
    if ($targetBase -eq "scripts") {
        return [pscustomobject]@{
            TargetPath = Join-Path $ScriptRoot $relativePath
            BackupRelativePath = $relativePath
        }
    }
    if ($targetBase -eq "data") {
        return [pscustomobject]@{
            TargetPath = Join-Path $DataRoot $relativePath
            BackupRelativePath = Join-Path "__data" $relativePath
        }
    }
    throw "Patch contains an unknown target base for $($Record.path): $targetBase"
}

function Find-VerifiedSourceBackup {
    param(
        [Parameter(Mandatory = $true)][string] $DataRoot,
        [Parameter(Mandatory = $true)][string] $RelativePath,
        [Parameter(Mandatory = $true)][string] $ExpectedSha256
    )

    $backupParent = Join-Path $DataRoot ".fetcher-bardcraft-backups"
    if (-not (Test-Path -LiteralPath $backupParent -PathType Container)) {
        return $null
    }

    $versionDirectories = Get-ChildItem -LiteralPath $backupParent -Directory | Sort-Object LastWriteTimeUtc -Descending
    foreach ($versionDirectory in $versionDirectories) {
        $candidate = Join-Path $versionDirectory.FullName $RelativePath
        if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and
            (Get-Sha256 -Path $candidate) -eq $ExpectedSha256) {
            return $candidate
        }
    }

    return $null
}

function Write-ReconstructedFile {
    param(
        [Parameter(Mandatory = $true)] $Record,
        [byte[]] $SourceBytes,
        [Parameter(Mandatory = $true)][string] $Destination
    )

    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $stream = [System.IO.File]::Open($Destination, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
    try {
        foreach ($operation in $Record.operations) {
            $propertyNames = @($operation.PSObject.Properties.Name)
            if ($propertyNames -contains "copyOffset") {
                $offset = [int64]$operation.copyOffset
                $length = [int]$operation.copyLength
                if ($offset -lt 0 -or $length -lt 0 -or $offset + $length -gt $SourceBytes.LongLength) {
                    throw "Patch contains an invalid source range for $($Record.path)."
                }
                $stream.Write($SourceBytes, [int]$offset, $length)
            }
            elseif ($propertyNames -contains "data") {
                $data = [Convert]::FromBase64String([string]$operation.data)
                $stream.Write($data, 0, $data.Length)
            }
            else {
                throw "Patch contains an unknown operation for $($Record.path)."
            }
        }
    }
    finally {
        $stream.Dispose()
    }
}

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $packageRoot "fetcher-bardcraft-mp-patch.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Patch manifest was not found: $manifestPath"
}

$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if ([int]$manifest.formatVersion -notin @(1, 2)) {
    throw "Unsupported Fetcher Bardcraft patch format: $($manifest.formatVersion)"
}

$dataRoot = (Resolve-Path -LiteralPath $BardcraftDataRoot).Path
$targetRoot = Join-Path $dataRoot ([string]$manifest.targetSubdirectory).Replace("/", "\")
if (-not (Test-Path -LiteralPath $targetRoot -PathType Container)) {
    throw "Bardcraft script directory was not found: $targetRoot"
}

$safeVersion = ([string]$manifest.patchVersion) -replace '[^A-Za-z0-9._-]', '_'
$workRoot = Join-Path $dataRoot ".fetcher-bardcraft-patch-work-$([Guid]::NewGuid().ToString('N'))"
$stageRoot = Join-Path $workRoot "stage"
$backupRoot = Join-Path $dataRoot ".fetcher-bardcraft-backups\$safeVersion"
$pending = New-Object System.Collections.Generic.List[object]

try {
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null

    foreach ($record in $manifest.files) {
        $location = Resolve-PatchRecordLocation -Record $record -DataRoot $dataRoot -ScriptRoot $targetRoot
        $relativePath = $location.BackupRelativePath
        $targetPath = $location.TargetPath
        $outputHash = ([string]$record.outputSha256).ToLowerInvariant()
        $sourceHash = if ($null -eq $record.sourceSha256) { $null } else { ([string]$record.sourceSha256).ToLowerInvariant() }
        $recordProperties = @($record.PSObject.Properties.Name)
        $priorOutputHashes = if ($recordProperties -contains "priorOutputSha256") {
            @($record.priorOutputSha256 | ForEach-Object { ([string]$_).ToLowerInvariant() })
        }
        else {
            @()
        }

        if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
            $currentHash = Get-Sha256 -Path $targetPath
            if ($currentHash -eq $outputHash) {
                Write-Output "Already patched: $($record.path)"
                continue
            }
            if ($null -ne $sourceHash -and $currentHash -eq $sourceHash) {
                $sourceBytes = [System.IO.File]::ReadAllBytes($targetPath)
            }
            elseif ($null -ne $sourceHash -and $priorOutputHashes -contains $currentHash) {
                $sourceBackup = Find-VerifiedSourceBackup -DataRoot $dataRoot -RelativePath $relativePath -ExpectedSha256 $sourceHash
                if ($null -eq $sourceBackup) {
                    throw "Cannot upgrade $($record.path): its verified pristine backup was not found. Reinstall upstream Bardcraft, then apply this patch again."
                }
                Write-Output "Upgrading from verified backup: $($record.path)"
                $sourceBytes = [System.IO.File]::ReadAllBytes($sourceBackup)
            }
            elseif ($null -eq $sourceHash -and $priorOutputHashes -contains $currentHash) {
                Write-Output "Upgrading previously added file: $($record.path)"
                $sourceBytes = [byte[]]@()
            }
            else {
                throw "Unsupported or modified Bardcraft file: $($record.path) (sha256=$currentHash)"
            }
        }
        else {
            if ($null -ne $sourceHash) {
                throw "Required Bardcraft file is missing: $($record.path)"
            }
            $sourceBytes = [byte[]]@()
        }

        $stagedPath = Join-Path $stageRoot $relativePath
        Write-ReconstructedFile -Record $record -SourceBytes $sourceBytes -Destination $stagedPath
        $stagedHash = Get-Sha256 -Path $stagedPath
        if ($stagedHash -ne $outputHash) {
            throw "Reconstructed output failed verification for $($record.path)."
        }

        $pending.Add([pscustomobject]@{
            RelativePath = $relativePath
            TargetPath = $targetPath
            StagedPath = $stagedPath
            Existed = Test-Path -LiteralPath $targetPath -PathType Leaf
            Record = $record
        })
    }

    if ($pending.Count -gt 0) {
        New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
        foreach ($item in $pending) {
            if ($item.Existed) {
                $backupPath = Join-Path $backupRoot $item.RelativePath
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backupPath) | Out-Null
                Copy-Item -LiteralPath $item.TargetPath -Destination $backupPath -Force
            }
        }

        $applied = New-Object System.Collections.Generic.List[object]
        try {
            foreach ($item in $pending) {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $item.TargetPath) | Out-Null
                $applied.Add($item)
                Copy-Item -LiteralPath $item.StagedPath -Destination $item.TargetPath -Force
                $record = $item.Record
                if ((Get-Sha256 -Path $item.TargetPath) -ne ([string]$record.outputSha256).ToLowerInvariant()) {
                    throw "Installed output failed verification for $($record.path)."
                }
            }

            foreach ($record in $manifest.files) {
                $location = Resolve-PatchRecordLocation -Record $record -DataRoot $dataRoot -ScriptRoot $targetRoot
                $targetPath = $location.TargetPath
                if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf) -or
                    (Get-Sha256 -Path $targetPath) -ne ([string]$record.outputSha256).ToLowerInvariant()) {
                    throw "Final verification failed for $($record.path)."
                }
            }

        }
        catch {
            foreach ($item in $applied) {
                $backupPath = Join-Path $backupRoot $item.RelativePath
                if ($item.Existed -and (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
                    Copy-Item -LiteralPath $backupPath -Destination $item.TargetPath -Force
                }
                elseif (-not $item.Existed -and (Test-Path -LiteralPath $item.TargetPath)) {
                    Remove-Item -LiteralPath $item.TargetPath -Force
                }
            }
            throw
        }
    }

    $marker = [ordered]@{
        patchVersion = [string]$manifest.patchVersion
        appliedAtUtc = [DateTime]::UtcNow.ToString("o")
        manifestSha256 = Get-Sha256 -Path $manifestPath
    }
    $stagedMarker = Join-Path $workRoot "fetcher-bardcraft-mp-patch.json"
    $marker | ConvertTo-Json | Set-Content -LiteralPath $stagedMarker -Encoding UTF8
    Move-Item -LiteralPath $stagedMarker -Destination (Join-Path $dataRoot "fetcher-bardcraft-mp-patch.json") -Force
    Write-Output "Fetcher Bardcraft multiplayer patch $($manifest.patchVersion) applied successfully."
}
finally {
    if (Test-Path -LiteralPath $workRoot) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
}
