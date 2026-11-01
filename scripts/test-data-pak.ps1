# Owned, temporary data-pack installation used by run-modded.ps1. No existing mod is overwritten.
function Install-Sm2TestDataPak([string]$Source, [string]$GameRoot, [string]$RunDirectory) {
    $targetPak = Join-Path $GameRoot 'mods\SM2VR_FirstPerson_Test.pak'
    if (Test-Path -LiteralPath $targetPak) { throw "Test data pack already exists: $targetPak" }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Data pack missing: $Source" }
    if (-not (Test-Path -LiteralPath (Split-Path $targetPak -Parent) -PathType Container)) { throw 'Game mods directory missing' }
    if (-not (Test-Path -LiteralPath $RunDirectory -PathType Container)) { throw 'Run directory missing' }
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $Source -Destination $targetPak -ErrorAction Stop
    $receipt = [pscustomobject]@{ Path = $targetPak; Sha256 = $sourceHash }
    try {
        $receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunDirectory 'test-data-pak.json') -ErrorAction Stop
        $sourceManifest = [IO.Path]::ChangeExtension($Source, '.json')
        if (Test-Path -LiteralPath $sourceManifest) {
            Copy-Item -LiteralPath $sourceManifest -Destination (Join-Path $RunDirectory 'camera-preset.json') -ErrorAction Stop
        }
    } catch {
        if ((Get-FileHash -LiteralPath $targetPak -Algorithm SHA256).Hash -eq $sourceHash) {
            Remove-Item -LiteralPath $targetPak -Force
        }
        throw
    }
    return $receipt
}

function Remove-Sm2TestDataPak($Receipt) {
    if ($null -eq $Receipt -or -not (Test-Path -LiteralPath $Receipt.Path)) { return $true }
    if ((Get-FileHash -LiteralPath $Receipt.Path -Algorithm SHA256).Hash -ne $Receipt.Sha256) {
        Write-Warning "Test pack changed during the run; left in place: $($Receipt.Path)"
        return $false
    }
    Remove-Item -LiteralPath $Receipt.Path -Force -ErrorAction Stop
    return -not (Test-Path -LiteralPath $Receipt.Path)
}
