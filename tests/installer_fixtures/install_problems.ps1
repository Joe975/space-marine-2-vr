# Prints Get-Sm2InstallProblems for a fixture install, as a JSON array.
param([string]$PackageRoot, [string]$GameRoot, [string]$StateRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
foreach ($part in 'sm2-game', 'sm2-install') { . (Join-Path $PSScriptRoot "..\..\installer\app\$part.ps1") }
$problems = @(Get-Sm2InstallProblems -InstallState (Read-Sm2InstallState $StateRoot) -Package (Get-Sm2Package $PackageRoot) -Game (Find-Sm2Game -Override $GameRoot))
ConvertTo-Json -InputObject $problems
