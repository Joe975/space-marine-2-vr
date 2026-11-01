# Prints Find-Sm2Game for a fixture Steam tree (and the exe's PE timestamp), as JSON.
param([string]$SteamPath, [string]$Override = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
. (Join-Path $PSScriptRoot '..\..\installer\app\sm2-game.ps1')
$game = Find-Sm2Game -SteamPath $SteamPath -Override $Override
if ($null -eq $game) { 'null'; return }
$game | Add-Member -NotePropertyName PeTimestamp -NotePropertyValue (Get-Sm2PeTimestamp $game.Exe)
$game | ConvertTo-Json
