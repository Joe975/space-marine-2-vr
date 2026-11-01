# Transactional ownership for the optional OpenVR SDK DLL. Never replace an unrelated file.
function Install-Sm2RuntimeApi([string]$Source, [string]$Target) {
    $sourcePath = (Resolve-Path -LiteralPath $Source -ErrorAction Stop).Path
    $targetPath = [IO.Path]::GetFullPath($Target)
    if ([IO.Path]::GetFileName($targetPath) -ne 'openvr_api.dll') { throw 'Unexpected runtime DLL target name' }
    $hash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
    $created = -not (Test-Path -LiteralPath $targetPath)
    if (-not $created) {
        if ((Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ne $hash) { throw 'An unrelated openvr_api.dll already exists' }
    } else { Copy-Item -LiteralPath $sourcePath -Destination $targetPath -ErrorAction Stop }
    [pscustomobject]@{ Path = $targetPath; Hash = $hash; Created = $created }
}
function Remove-Sm2RuntimeApi($Receipt) {
    if (-not $Receipt -or -not $Receipt.Created) { return $true }
    if (-not (Test-Path -LiteralPath $Receipt.Path)) { return $true }
    if ((Get-FileHash -LiteralPath $Receipt.Path -Algorithm SHA256).Hash -ne $Receipt.Hash) {
        Write-Warning 'Runtime DLL changed after installation; preserving it'
        return $false
    }
    Remove-Item -LiteralPath $Receipt.Path -Force -ErrorAction Stop
    return -not (Test-Path -LiteralPath $Receipt.Path)
}
