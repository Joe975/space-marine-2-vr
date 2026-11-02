# Prints the installer's launch environment and control files for a profile, as JSON.
param([string]$ProfilePath, [string]$ConfigDirectory, [string]$DumpDirectory, [switch]$Chase, [string]$MainHand)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
. (Join-Path $PSScriptRoot '..\..\installer\app\sm2-profile.ps1')
$launch = ConvertTo-Sm2Launch -Profile (Set-Sm2MainHand (Read-Sm2Profile $ProfilePath) $MainHand) -ConfigDirectory $ConfigDirectory -DumpDirectory $DumpDirectory -Chase:$Chase
@{ env = $launch.Env; files = $launch.Files } | ConvertTo-Json
