# Prints Test-Sm2PackageAudit for a fixture package folder, as a JSON array.
param([string]$Root)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
. (Join-Path $PSScriptRoot '..\..\scripts\installer-package.ps1')
ConvertTo-Json -InputObject @(Test-Sm2PackageAudit $Root)
