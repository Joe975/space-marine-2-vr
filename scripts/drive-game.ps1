# Drives a running modded game through its menus with keyboard input, reading the mod log to know
# where it is. Start the game first (run-ordinary-headset.ps1 without -AutoContinue, or the installer),
# then run this alongside it. docs/GAME_AUTOMATION.md has the recipes and what the menus look like.
#
# Usage: drive-game.ps1 -Do 'continue,quit-title' [-GamePid N] [-OutDir dir]
#   continue        startup screens and the main menu's Continue, then wait until the level has loaded
#                   (the mod has seen the player for 15 s, or the first stereo pair). Run it right after launch.
#   pause:<Item>    open the pause menu and choose an item: Continue, LoadLastCheckpoint, Settings,
#                   PhotoMode, QuitToTitle, Quit (Battle Barge hangar order, scripts/game-menus.ps1)
#   quit-title      pause:QuitToTitle, accept the confirmation, then wait for the title or a hang
#   keys:ESC+DOWN   press keys in order, 0.7 s apart (F8 cycles the camera mode, F9 recenters)
#   hold:W:6        hold one key for that many seconds (up to 30), e.g. walk the Marine forward
#   eyes:<name>     save both submitted eye images to <EyeDir>\<name>\ (launch with -EyeSnapshots);
#                   names are letters, digits, _ and -, each used once, at most 24 per launch
#   wait:<seconds>  wait
#   shot:<name>     screenshot the game window to <OutDir>\<name>.png (skipped if the game is hung)
# -RequireStereo: after continue, exit 3 unless the mod's OpenXR session is running (with nobody in the
# headset SteamVR is usually in standby and the session never starts; relaunch and try again).
# Exit code 0 when every step finished, 2 when the game hung, 3 no VR session (-RequireStereo), 1 otherwise.
# Keys only reach the game while its window is in front; this script brings it forward when needed,
# so leave the keyboard and mouse alone while it runs.
# -Launch "-Controls -Gestures -TestDriver -ScriptedHead -Seconds 900 -Tag x": start
# run-ordinary-headset.ps1 with those arguments first (no -AutoContinue or -DirectLaunch; values
# without spaces) and drive the game it starts; its output goes to <OutDir>\launcher.txt.
# -Close: end the game when the steps are done, so the launcher removes the mod files straight away
# instead of at its -Seconds deadline.
# -EyeDir: the launch's dump directory for eyes steps. Default with -Launch: build\ordinary\<today>\<its -Tag>,
# where run-ordinary-headset.ps1 puts it.
param([Parameter(Mandatory)][string]$Do, [int]$GamePid = 0, [string]$OutDir = '', [switch]$RequireStereo,
    [string]$Launch = '', [switch]$Close, [double]$ContinueTimeout = 600, [double]$ExitTimeout = 120, [string]$EyeDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'auto-continue.ps1')
. (Join-Path $PSScriptRoot 'game-menus.ps1')
. (Join-Path $PSScriptRoot 'game-keys.ps1')

if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) ('build\drive\' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not $EyeDir -and $Launch -match '(^|\s)-Tag\s+(\S+)') {
    $EyeDir = Join-Path (Split-Path -Parent $PSScriptRoot) ('build\ordinary\' + (Get-Date -Format 'yyyy-MM-dd') + '\' + $Matches[2])
}
if ($Launch) {
    if ($Launch -match '(^|\s)-(AutoContinue|DirectLaunch|InGameKeys)\b') { throw '-Launch must not use -AutoContinue, -DirectLaunch or -InGameKeys: the driver does the menus.' }
    if ($GamePid) { throw 'Pass -Launch or -GamePid, not both.' }
    $launcherLog = Join-Path $OutDir 'launcher.txt'
    $launcherArgs = "-NoProfile -ExecutionPolicy Bypass -Command `"& '$(Join-Path $PSScriptRoot 'run-ordinary-headset.ps1')' $Launch *>&1 | Out-File -Encoding utf8 '$launcherLog'`""
    $launcher = Start-Process powershell.exe -ArgumentList $launcherArgs -WindowStyle Hidden -PassThru
    Write-Host "launcher pid $($launcher.Id), output $launcherLog"
    $wait = (Get-Date).AddSeconds(120)
    while (-not $GamePid -and (Get-Date) -lt $wait) {
        Start-Sleep -Seconds 1
        # An empty log reads as $null, which PowerShell 5.1 passes to Match as null even through [string].
        $text = if (Test-Path -LiteralPath $launcherLog) { Get-Content -LiteralPath $launcherLog -Raw } else { $null }
        if ($text) { $m = [regex]::Match($text,'owned game: pid=(\d+)'); if ($m.Success) { $GamePid = [int]$m.Groups[1].Value } }
        if ($launcher.HasExited -and -not $GamePid) { throw "The launcher exited without starting the game; see $launcherLog." }
    }
    if (-not $GamePid) { throw "The launcher did not start the game within 120 s; see $launcherLog." }
}
if (-not $GamePid) {
    $games = @(Get-Process | Where-Object ProcessName -eq 'Warhammer 40000 Space Marine 2 - Retail')
    if ($games.Count -ne 1) { throw "Pass -GamePid: found $($games.Count) game processes." }
    $GamePid = $games[0].Id
}
$log = Join-Path $env:LOCALAPPDATA 'sm2vr\sm2vr.log'
$reader = New-Sm2AutoContinue   # only its log-tail offset is used outside 'continue'
$vrSession = ''
$start = Get-Date
# Progress goes to the host, so it never mixes into a function's return value.
function Say([string]$text) { Write-Host "t+$([int]((Get-Date) - $start).TotalSeconds)s $text" }
function Alive { [bool](Get-Process -Id $GamePid -ErrorAction SilentlyContinue) }
function Press([string[]]$names) {
    foreach ($name in $names) {
        if (-not (Set-Sm2GameForeground -GamePid $GamePid)) { throw "The game window could not be brought to the front for $name." }
        if (-not (Send-Sm2Key -Name $name -GamePid $GamePid)) { throw "$name was not sent (game window not in front)." }
        Start-Sleep -Milliseconds 700
    }
}
function Shot([string]$name) {
    $p = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
    if (-not $p -or -not $p.Responding) { return Say "shot $name skipped (game not responding)" }
    $path = Join-Path $OutDir "$name.png"
    & (Join-Path $PSScriptRoot 'screenshot.ps1') -Out $path -ProcessId $GamePid | Out-Null
    Say "shot $path"
}
function Hold([string]$text) {
    $hold = ConvertFrom-Sm2HoldStep $text
    if (-not (Set-Sm2GameForeground -GamePid $GamePid)) { throw "The game window could not be brought to the front for hold:$text." }
    if (-not (Send-Sm2KeyHold -Name $hold.Key -GamePid $GamePid -Seconds $hold.Seconds)) { throw "hold:$text was cut short (game window not in front)." }
}
# The mod polls <EyeDir>\request.txt once a second and dumps the next stereo pair into <EyeDir>\<name>\.
# A missing pair (no stereo then, or a reused name) is reported, not thrown: the run goes on.
function Eyes([string]$name) {
    if (-not $EyeDir) { throw "eyes:$name needs -EyeDir (or -Launch with -Tag)." }
    if ($name -notmatch '^[A-Za-z0-9_-]{1,40}$') { throw "eyes:${name}: use letters, digits, _ and - (at most 40)." }
    New-Item -ItemType Directory -Force -Path $EyeDir | Out-Null
    $temporary = Join-Path $EyeDir ([guid]::NewGuid().ToString('N') + '.tmp')
    [IO.File]::WriteAllText($temporary, "$name`n", [Text.Encoding]::ASCII)
    Move-Item -LiteralPath $temporary -Destination (Join-Path $EyeDir 'request.txt') -Force
    $pair = Join-Path $EyeDir "$name\pair.json"; $deadline = (Get-Date).AddSeconds(10)
    while (-not (Test-Path -LiteralPath $pair) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    if (Test-Path -LiteralPath $pair) { Say "eyes $(Split-Path -Parent $pair)" } else { Say "eyes $name MISSING (no stereo pair within 10 s)" }
}
# Startup Enter: skipped (not thrown) while the window isn't up yet or can't come to the front.
function TryEnter { if ((Set-Sm2GameForeground -GamePid $GamePid) -and (Send-Sm2Key -Name ENTER -GamePid $GamePid)) { Start-Sleep -Milliseconds 200 } }
function Invoke-Continue {
    # Enter every second from 5 s: Enter is harmless on every startup screen and on the main menu
    # (nothing is selected there), so only the one Down needs the flags (auto-continue.ps1).
    $state = New-Sm2AutoContinue -FirstAt 5 -Interval 1 -MaxPresses 900; $state.Offset = $reader.Offset
    $settle = New-Sm2InGame; $continued = $false; $check = $null
    $deadline = (Get-Date).AddSeconds($ContinueTimeout)
    while ((Get-Date) -lt $deadline) {
        if (-not (Alive)) { throw 'The game exited during continue.' }
        Start-Sleep -Milliseconds 500
        $t = ((Get-Date) - $start).TotalSeconds
        $tail = Read-Sm2LogTail $state $log
        $session = Get-Sm2VrSession $tail; if ($session) { $script:vrSession = $session }
        if (-not $continued) {
            $press = Step-Sm2AutoContinue $state $tail $t
            if ($press -eq 'enter') { TryEnter }
            elseif ($press -eq 'down-enter') { Press 'DOWN', 'ENTER'; $continued = $true; $check = New-Sm2ContinueCheck -Since $t; Say 'main menu: Continue' }
            elseif ($state.Done -and $null -ne $state.InGameAt) { $continued = $true; Say 'in game (first stereo pair during startup)'; break }
            elseif ($state.Done) { throw "Auto-continue gave up after $($state.Presses) presses without reaching the main menu." }
        } elseif ($tail.Contains($Sm2InGameMarker)) { Say 'in game (first stereo pair)'; break }
        if ($check) {
            switch (Step-Sm2ContinueCheck $check $tail $t) {
                'enter' { Say 'Continue did not start: Enter'; Press 'ENTER' }
                'down-enter' { Say 'Continue did not start: Down, Enter'; Press 'DOWN', 'ENTER' }
                'give-up' { Shot 'continue-stuck'; throw 'Continue did not start after two retries (see continue-stuck.png).' }
            }
        }
        if ($continued -and (Step-Sm2InGame $settle $tail $t)) { Say "in game (player present $($settle.SettleSeconds) s)"; break }
    }
    $reader.Offset = $state.Offset
    if ((Get-Date) -ge $deadline) { throw "Not in game after $ContinueTimeout s." }
    # The session can start just after the first pair (16:33, 0.1.3): give it 30 s.
    $sessionDeadline = (Get-Date).AddSeconds(30)
    while ($RequireStereo -and $script:vrSession -ne 'ready' -and (Get-Date) -lt $sessionDeadline) {
        Start-Sleep -Milliseconds 500
        $session = Get-Sm2VrSession (Read-Sm2LogTail $reader $log); if ($session) { $script:vrSession = $session }
    }
    if ($RequireStereo -and $script:vrSession -ne 'ready') { Say "no VR session ($script:vrSession): stereo never started"; return 3 }
    0
}
function Invoke-QuitTitle {
    # Only what is logged after the quit counts: the level load before it publishes 0x1400 too, and
    # a 'wait' step leaves that unread (16:20, a false "title" on a frozen game).
    $null = Read-Sm2LogTail $reader $log
    Press (Get-Sm2PauseMenuKeys 'QuitToTitle'); Press 'ENTER'; Start-Sleep -Milliseconds 1500; Press 'ENTER'
    Say 'quit to title accepted'
    $deadline = (Get-Date).AddSeconds($ExitTimeout)
    $watch = New-Sm2ExitWatch -Since ((Get-Date) - $start).TotalSeconds
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $p = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
        if (-not $p) { Say 'the game exited'; return 1 }
        switch (Step-Sm2ExitWatch $watch (Read-Sm2LogTail $reader $log) ((Get-Date) - $start).TotalSeconds $p.Responding) {
            'hang' { Say 'HANG: no frame (the mod watchdog fired, or no log and no response for 20 s)'; return 2 }
            'title' { Say 'title screen reached'; return 0 }
        }
    }
    Say "neither the title nor a hang after $ExitTimeout s"; 1
}

$code = 0
try {
    $null = Read-Sm2LogTail $reader $log   # skip what was logged before this script started
    if ($Do -match '(^|,)\s*continue\s*(,|$)') { $reader.Offset = 0 }   # continue reads the launch from its start
    foreach ($step in ($Do -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $name, $arg = $step -split ':', 2
        Say "step $step"
        switch ($name) {
            'continue' { $code = Invoke-Continue }
            'pause' { Press (Get-Sm2PauseMenuKeys $arg); Press 'ENTER' }
            'quit-title' { $code = Invoke-QuitTitle }
            'keys' { Press ($arg -split '\+') }
            'hold' { Hold $arg }
            'eyes' { Eyes $arg }
            'wait' { Start-Sleep -Seconds ([double]$arg) }
            'shot' { Shot $arg }
            default { throw "Unknown step '$step'." }
        }
        if ($code) { break }
    }
} catch { Say "FAILED: $($_.Exception.Message)"; $code = 1 }
$game = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
if ($code -eq 2 -and $game) {
    # The watchdog keeps one dump per process, so take every thread's stack now (non-invasive).
    $cdb = Get-Command cdbX64.exe -ErrorAction SilentlyContinue
    if ($cdb) {
        $stacks = Join-Path $OutDir 'hang-stacks.txt'
        # A command file: Windows PowerShell mangles quotes inside a -c argument to a native program.
        $commands = Join-Path $OutDir 'hang-stacks.cdb'; Set-Content -LiteralPath $commands -Encoding ascii -Value '.sympath ""', '~* kn 12', 'q'
        & $cdb.Source -pv -p $GamePid -cf $commands 2>$null | Out-File -Encoding utf8 $stacks
        Say "thread stacks: $stacks"
    } else { Say 'no cdbX64.exe (WinDbg): thread stacks not taken' }
}
if ($Close -and $game) { Stop-Process -Id $GamePid -Force; Say 'closed the game; the launcher removes the mod files' }
Say "done (exit $code)"
exit $code
