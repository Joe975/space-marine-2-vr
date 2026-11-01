# Removes the mod's files from the game folder (the dxgi.dll proxy and the files the framework drops).
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'installer\app\sm2-game.ps1')
$game = Find-Sm2Game
if (-not $game) { throw 'Space Marine 2 was not found through Steam; set SM2VR_GAME_ROOT to the game folder.' }
$gameBin = $game.Bin
if (Get-Process -Name 'Warhammer 40000 Space Marine 2 - Retail' -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
foreach ($f in 'dxgi.dll', 'vr_config.txt', 'vr_ui.ini', 'ref_ui.ini', 'vr_log.txt', 'vrframework_crash.dmp', 'imgui.ini') {
    $p = Join-Path $gameBin $f
    if (Test-Path $p) { Remove-Item $p -Force; "removed $p" }
}
'game folder is clean of mod files'
