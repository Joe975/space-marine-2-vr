param([string]$Work)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts/runtime-api.ps1')
$source = Join-Path $Work 'sdk.dll'
$target = Join-Path $Work 'openvr_api.dll'
[IO.File]::WriteAllText($source,'sdk fixture')
$receipt = Install-Sm2RuntimeApi $source $target
if (-not $receipt.Created -or -not (Test-Path -LiteralPath $target)) { throw 'owned installation failed' }
$borrowed = Install-Sm2RuntimeApi $source $target
if ($borrowed.Created -or -not (Remove-Sm2RuntimeApi $borrowed) -or -not (Test-Path -LiteralPath $target)) { throw 'borrowed runtime must survive cleanup' }
if (-not (Remove-Sm2RuntimeApi $receipt) -or (Test-Path -LiteralPath $target)) { throw 'owned runtime must be removed' }
[IO.File]::WriteAllText($target,'another runtime')
$refused = $false
try { Install-Sm2RuntimeApi $source $target | Out-Null } catch { $refused = $true }
if (-not $refused -or [IO.File]::ReadAllText($target) -ne 'another runtime') { throw 'unrelated runtime was overwritten' }
Remove-Item -LiteralPath $target
$receipt = Install-Sm2RuntimeApi $source $target
[IO.File]::WriteAllText($target,'modified after installation')
if ((Remove-Sm2RuntimeApi $receipt) -or -not (Test-Path -LiteralPath $target)) { throw 'modified owned runtime must survive cleanup' }
'runtime API ownership passed'
