# Runs Install-Sm2 against a fixture package, game and Saber folder; prints the install state as JSON.
param([string]$PackageRoot, [string]$GameRoot, [string]$StateRoot, [string]$SaberRoot, [string]$Python)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
foreach ($part in 'sm2-game', 'sm2-install') { . (Join-Path $PSScriptRoot "..\..\installer\app\$part.ps1") }
$package = Get-Sm2Package $PackageRoot
# Stand-in for the bundled interpreter.
$package.Python = $Python
$game = Find-Sm2Game -Override $GameRoot
$state = Install-Sm2 -Package $package -Game $game -StateRoot $StateRoot -SaberRoot $SaberRoot
@{ state = $state; problems = @(Get-Sm2InstallProblems -InstallState (Read-Sm2InstallState $StateRoot) -Package $package -Game $game) } | ConvertTo-Json -Depth 6
