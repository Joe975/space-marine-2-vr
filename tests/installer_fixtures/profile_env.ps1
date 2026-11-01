# Prints the installer's launch environment and control files for a profile, as JSON.
param([string]$ProfilePath, [string]$ConfigDirectory, [string]$DumpDirectory, [switch]$Chase)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
. (Join-Path $PSScriptRoot '..\..\installer\app\sm2-profile.ps1')
$launch = ConvertTo-Sm2Launch -Profile (Read-Sm2Profile $ProfilePath) -ConfigDirectory $ConfigDirectory -DumpDirectory $DumpDirectory -Chase:$Chase
@{ env = $launch.Env; files = $launch.Files } | ConvertTo-Json
