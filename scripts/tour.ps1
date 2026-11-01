# The scripted tour: one fixed route through the game per build, then the run report on its log.
# Joe's playtests should confirm a build, not be where its menu and stereo bugs are found
# (docs/GENERIC_GUARDS_2026-10-26.md). Needs the game, SteamVR and nobody at the keyboard or mouse.
#
# Usage: tour.ps1 [-Tag name] [-Extra "-EnvOff SM2VR_X"] [-Learn]
#   -Extra   more run-ordinary-headset.ps1 arguments (values without spaces), e.g. to bisect a switch
#   -Learn   add the run's NEW states to config/known_run_states.json (read the report first)
# Exit code: drive-game.ps1's code when it failed (2 hang, 3 no VR session, 1 other), else the
# report's (1 for any verdict or NEW state, 0 clean).
#
# The route, all from the save in the Battle Barge hangar (docs/GAME_AUTOMATION.md):
#   startup screens, title, main menu, Continue (loads 0x1000/0x1400/0x1800/0xc00/0x800)
#   the hangar for 30 s (the walkable barge, 0x800: must stay in stereo)
#   the pause menu and back through its Continue (0x908 on the panel, then stereo again)
#   quit to title (the level-exit freeze of 24-25 Oct)
# TODO once their menu positions are read off screenshots and added to scripts/game-menus.ps1 with a
# test: the battle simulator (25 Oct: no gun), mission select, a mission load (the loading screen,
# a cutscene), return to the Battle Barge from a mission's pause menu.
param([string]$Tag = 'tour', [string]$Extra = '', [switch]$Learn)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root ('build\tour\' + (Get-Date -Format 'yyyyMMdd_HHmmss') + "_$Tag")
New-Item -ItemType Directory -Force -Path $out | Out-Null
$route = 'continue,shot:hangar,wait:30,shot:hangar_later,keys:ESC,wait:3,shot:pause,keys:DOWN+ENTER,wait:10,shot:unpaused,quit-title,shot:title'
$launch = "-Controls -Gestures -TestDriver -ScriptedHead -Seconds 1200 -Tag $Tag $Extra".Trim()
Write-Host "tour: $route"
& (Join-Path $PSScriptRoot 'drive-game.ps1') -Launch $launch -Do $route -RequireStereo -Close -OutDir $out
$drive = $LASTEXITCODE
# The live log is complete once the game has gone; keep it with the tour's screenshots.
$live = Join-Path $env:LOCALAPPDATA 'sm2vr\sm2vr.log'
if (Test-Path -LiteralPath $live) { Copy-Item -LiteralPath $live -Destination (Join-Path $out 'sm2vr.log') -Force }
$python = Join-Path $root '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) { $python = 'python' }
$reportArgs = @((Join-Path $PSScriptRoot 'run_report.py'), $out, '--strict')
if ($Learn) { $reportArgs += '--learn' }
& $python @reportArgs | Tee-Object -FilePath (Join-Path $out 'report.txt')
$report = $LASTEXITCODE
Write-Host "tour: drive exit $drive, report exit $report, output $out"
if ($drive -ne 0) { exit $drive }
exit $report
