$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $repoRoot 'scripts\test-data-pak.ps1')
$scratch = Join-Path $repoRoot ('build\test-data-pak-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path (Join-Path $scratch 'game\mods') -Force
$null = New-Item -ItemType Directory -Path (Join-Path $scratch 'run') -Force
$source = Join-Path $scratch 'source.pak'
$sourceManifest = Join-Path $scratch 'source.json'
$target = Join-Path $scratch 'game\mods\SM2VR_FirstPerson_Test.pak'
function Assert($condition, $message) { if (-not $condition) { throw $message } }
try {
    Set-Content -LiteralPath $source -Value 'fixture source'
    Set-Content -LiteralPath $sourceManifest -Value '{"head_height":2.5}'
    $receipt = Install-Sm2TestDataPak $source (Join-Path $scratch 'game') (Join-Path $scratch 'run')
    Assert (Test-Path -LiteralPath $target) 'pack was not installed'
    Assert (Test-Path -LiteralPath (Join-Path $scratch 'run\camera-preset.json')) 'preset was not recorded'
    $refused = $false
    try { Install-Sm2TestDataPak $source (Join-Path $scratch 'game') (Join-Path $scratch 'run') } catch { $refused = $true }
    Assert $refused 'pre-existing pack was overwritten'
    Set-Content -LiteralPath $target -Value 'changed by another actor'
    Assert (-not (Remove-Sm2TestDataPak $receipt)) 'changed pack was removed'
    Assert (Test-Path -LiteralPath $target) 'changed pack should remain'
    # The test owns this fixture and deliberately restores it to the receipt's content.
    Copy-Item -LiteralPath $source -Destination $target -Force
    Assert (Remove-Sm2TestDataPak $receipt) 'owned pack cleanup failed'
    Assert (-not (Test-Path -LiteralPath $target)) 'owned pack remained'
    Assert (Remove-Sm2TestDataPak $receipt) 'cleanup should be idempotent'
    $refused = $false
    try { Install-Sm2TestDataPak (Join-Path $scratch 'missing.pak') (Join-Path $scratch 'game') (Join-Path $scratch 'run') } catch { $refused = $true }
    Assert $refused 'missing source was accepted'
    $refused = $false
    try { Install-Sm2TestDataPak $source (Join-Path $scratch 'game') (Join-Path $scratch 'missing-run') } catch { $refused = $true }
    Assert $refused 'missing run directory was accepted'
    Assert (-not (Test-Path -LiteralPath $target)) 'failed setup left a pack installed'
    # Force receipt writing to fail after copying the pack; verify transactional rollback.
    $null = New-Item -ItemType Directory -Path (Join-Path $scratch 'blocked-run\test-data-pak.json') -Force
    $refused = $false
    try { Install-Sm2TestDataPak $source (Join-Path $scratch 'game') (Join-Path $scratch 'blocked-run') } catch { $refused = $true }
    Assert $refused 'receipt write failure was ignored'
    Assert (-not (Test-Path -LiteralPath $target)) 'receipt failure left a pack installed'
    'test-data-pak: PASS'
} finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build')) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside repo build directory' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
