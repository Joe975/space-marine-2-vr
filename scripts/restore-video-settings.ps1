# Restores the game's video settings after a launch with -Resolution/-WindowMode/-RenderResolution
# could not clean up (game left running, crash, refused restore). Launches refuse until this succeeds.
param([string]$PendingPath = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'game-video-settings.ps1')
if (-not $PendingPath) { $PendingPath = Get-Sm2VideoSettingsPendingPath }
if (Get-Process -Name 'Warhammer 40000 Space Marine 2 - Retail' -ErrorAction SilentlyContinue) { throw 'Close the game first; it rewrites its settings files on exit.' }
if (-not (Test-Path -LiteralPath $PendingPath)) { "no pending video-setting override ($PendingPath)"; return }
if (Restore-Sm2PendingVideoSettings $PendingPath) { 'video settings restored to their pre-launch bytes' }
else { throw "Video settings could not be fully restored; marker kept at $PendingPath" }
