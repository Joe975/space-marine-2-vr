# Runs Test-Sm2Preflight on a fixture state (JSON file) and prints the checks plus the verdict.
param([string]$StatePath, [string]$For = 'install')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
. (Join-Path $PSScriptRoot '..\..\installer\app\sm2-game.ps1')
. (Join-Path $PSScriptRoot '..\..\installer\app\sm2-preflight.ps1')
$checks = Test-Sm2Preflight -State ([IO.File]::ReadAllText($StatePath) | ConvertFrom-Json) -For $For
@{ passed = (Test-Sm2PreflightPassed $checks); checks = @($checks) } | ConvertTo-Json -Depth 4
