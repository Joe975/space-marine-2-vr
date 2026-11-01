# Prints Test-Sm2ReleaseAuthorized for a fixture DISTRIBUTION.md and repo visibility, as JSON.
param([string]$DistributionDoc, [string]$Visibility)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
# Load only the gate function: running publish-release.ps1 itself would build and publish.
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\..\scripts\publish-release.ps1'))
$match = [regex]::Match($source, '(?ms)^function Test-Sm2ReleaseAuthorized \{.*?^\}')
if (-not $match.Success) { throw 'Test-Sm2ReleaseAuthorized not found in publish-release.ps1' }
. ([scriptblock]::Create($match.Value))
ConvertTo-Json -InputObject @(Test-Sm2ReleaseAuthorized -DistributionDoc $DistributionDoc -Visibility $Visibility)
