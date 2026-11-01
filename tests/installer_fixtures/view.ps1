# Prints Get-Sm2StepView for fixture install/play states, as JSON.
param([string]$InstallStatePath, [string]$PlayStatePath, [string]$Problems = '', [switch]$BindingsCurrent, [switch]$Playing)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
foreach ($part in 'sm2-game', 'sm2-preflight', 'sm2-install', 'sm2-view') { . (Join-Path $PSScriptRoot "..\..\installer\app\$part.ps1") }
$install = Test-Sm2Preflight -State ([IO.File]::ReadAllText($InstallStatePath) | ConvertFrom-Json) -For install
$play = Test-Sm2Preflight -State ([IO.File]::ReadAllText($PlayStatePath) | ConvertFrom-Json) -For play
$problemList = @($Problems -split ';' | Where-Object { $_ })
ConvertTo-Json -Depth 4 -InputObject @(Get-Sm2StepView -InstallChecks $install -PlayChecks $play -InstallProblems $problemList -BindingsCurrent ([bool]$BindingsCurrent) -Playing ([bool]$Playing))
