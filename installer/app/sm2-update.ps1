# Updates: the release manifest names one zip per channel; the whole portable folder is replaced.
# Get-Sm2UpdatePlan is pure (tests feed it manifests); Install-Sm2Update swaps folders with rollback.

function ConvertTo-Sm2Version([string]$Text) {
    $v = $null
    if (-not [Version]::TryParse($Text, [ref]$v)) { throw "Not a version: '$Text'" }
    return $v
}

# Which release this install should move to. Beta is offered only while newer than stable.
function Get-Sm2UpdatePlan {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)][string]$CurrentVersion, [string]$Channel = 'stable', [string]$GameExeTimestamp)
    if ($Manifest.schema -ne 1) { throw "Unsupported release manifest schema: $($Manifest.schema)" }
    $stable = $Manifest.channels.stable
    $beta = $Manifest.channels.beta
    $pick = $stable
    if ($Channel -eq 'beta' -and $beta -and (-not $stable -or (ConvertTo-Sm2Version $beta.version) -gt (ConvertTo-Sm2Version $stable.version))) { $pick = $beta }
    if (-not $pick) { return [pscustomobject]@{ Action = 'none'; Message = 'No release is published yet.'; Release = $null } }
    $current = ConvertTo-Sm2Version $CurrentVersion
    $target = ConvertTo-Sm2Version $pick.version
    if ($target -eq $current) { return [pscustomobject]@{ Action = 'none'; Message = "Up to date ($CurrentVersion)."; Release = $pick } }
    if ($target -lt $current -and $Channel -ne 'stable') { return [pscustomobject]@{ Action = 'none'; Message = "Up to date ($CurrentVersion)."; Release = $pick } }
    $verb = $(if ($target -gt $current) { 'Update' } else { 'Switch' })
    $note = ''
    if ($GameExeTimestamp -and $pick.game_exe_timestamp -and $pick.game_exe_timestamp -ne $GameExeTimestamp) {
        $note = ' It supports a different game version than the one installed; update the game in Steam too.'
    }
    return [pscustomobject]@{ Action = 'update'; Message = "$verb to $($pick.version).$note"; Release = $pick }
}

function Get-Sm2Manifest([string]$Url) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    return (Invoke-WebRequest -Uri $Url -UseBasicParsing -TimeoutSec 30).Content | ConvertFrom-Json
}

function Save-Sm2Release {
    param([Parameter(Mandatory)]$Release, [Parameter(Mandatory)][string]$Directory)
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $zip = Join-Path $Directory ("SM2VR-$($Release.version).zip")
    Invoke-WebRequest -Uri $Release.url -OutFile $zip -UseBasicParsing -TimeoutSec 600
    return $zip
}

# Top-level items every release has; an extracted zip without them is refused.
$Sm2PackageItems = @('SM2VR.cmd', 'version.json', 'app', 'mod', 'builders', 'python')

# Replaces the package in place from a downloaded zip. The folder itself is not renamed (the player's
# shortcut and console may point into it); its top-level items are swapped, with rollback.
function Install-Sm2Update {
    param([Parameter(Mandatory)][string]$Zip, [Parameter(Mandatory)][string]$Sha256, [Parameter(Mandatory)][string]$PackageRoot)
    $actual = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash
    if ($actual -ne $Sha256.ToUpperInvariant()) { throw "Download is damaged (sha256 $($actual.ToLowerInvariant()), expected $($Sha256.ToLowerInvariant()))." }
    $stamp = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $incoming = Join-Path $PackageRoot (".update-$stamp")
    $retired = Join-Path $PackageRoot (".previous-$stamp")
    Expand-Archive -LiteralPath $Zip -DestinationPath $incoming
    try {
        # A zip may hold the package folder itself or its contents.
        $source = $incoming
        $children = @(Get-ChildItem -LiteralPath $incoming)
        if ($children.Count -eq 1 -and $children[0].PSIsContainer) { $source = $children[0].FullName }
        foreach ($item in $Sm2PackageItems) {
            if (-not (Test-Path -LiteralPath (Join-Path $source $item))) { throw "The release zip has no $item; not installing it." }
        }
        New-Item -ItemType Directory -Path $retired | Out-Null
        $movedOut = New-Object System.Collections.Generic.List[string]
        $movedIn = New-Object System.Collections.Generic.List[string]
        try {
            # Only what a release holds: a player's own files beside SM2VR.cmd stay.
            foreach ($name in @($Sm2PackageItems + @(Get-ChildItem -LiteralPath $source | ForEach-Object { $_.Name })) | Select-Object -Unique) {
                $path = Join-Path $PackageRoot $name
                if (-not (Test-Path -LiteralPath $path)) { continue }
                Move-Item -LiteralPath $path -Destination (Join-Path $retired $name)
                $movedOut.Add($name)
            }
            foreach ($item in @(Get-ChildItem -LiteralPath $source)) {
                Move-Item -LiteralPath $item.FullName -Destination (Join-Path $PackageRoot $item.Name)
                $movedIn.Add($item.Name)
            }
        } catch {
            $failure = $_
            foreach ($name in $movedIn) { Remove-Item -LiteralPath (Join-Path $PackageRoot $name) -Recurse -Force }
            foreach ($name in $movedOut) { Move-Item -LiteralPath (Join-Path $retired $name) -Destination (Join-Path $PackageRoot $name) }
            Remove-Item -LiteralPath $retired -Recurse -Force
            throw "Update failed and was rolled back: $($failure.Exception.Message)"
        }
        Remove-Item -LiteralPath $retired -Recurse -Force -ErrorAction SilentlyContinue
    } finally {
        if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
    }
    return ([IO.File]::ReadAllText((Join-Path $PackageRoot 'version.json')) | ConvertFrom-Json).mod_version
}
