# What drive-game.ps1 knows about the game's menus, as pure functions (tests/test_game_menus.py).
# Dot-source this file. The mod logs the native menu flags every 5 s ("CinematicState: ... published=0x..");
# docs/GAME_AUTOMATION.md lists the flags and screens these rules were read from (25 October 2026).

# Pause menu in the Battle Barge hangar, top to bottom. It opens with nothing selected, so the first
# Down selects the first item: item i needs i+1 Downs. "Quit to title screen" asks to confirm.
# Two variants: on 30 Oct (the hangar straight after Continue, tour pause.png) it had no "Load last
# checkpoint", and the 25 Oct position of "Quit to title" chose Quit, which closed the game; on 25 Oct
# it had one (-Checkpoint). Which the game shows when is not known yet.
$Sm2PauseMenu = @('Continue', 'Settings', 'PhotoMode', 'QuitToTitle', 'Quit')
$Sm2PauseMenuCheckpoint = @('Continue', 'LoadLastCheckpoint', 'Settings', 'PhotoMode', 'QuitToTitle', 'Quit')
$Sm2Flags = @{ Title = '0x1400'; Pause = '0x908' }

# Keys that open the pause menu and select an item (Enter not included).
function Get-Sm2PauseMenuKeys {
    param([Parameter(Mandatory)][string]$Item, [switch]$Checkpoint)
    $menu = if ($Checkpoint) { $Sm2PauseMenuCheckpoint } else { $Sm2PauseMenu }
    $index = [array]::IndexOf($menu, $Item)
    if ($index -lt 0) { throw "Unknown pause menu item '$Item'. Known: $($menu -join ', ')" }
    @('ESC') + @('DOWN') * ($index + 1)
}

# The menu flags published in new log text, oldest first.
function Get-Sm2PublishedFlags {
    param([string]$Text = '')
    @([regex]::Matches($Text, 'CinematicState: .*?published=(0x[0-9a-f]+)') | ForEach-Object { $_.Groups[1].Value })
}

# In game once the mod has seen the player's body (BodyFollowing actor=<non-zero>, logged every 5 s)
# for SettleSeconds with no line that lost it. Works with or without stereo, unlike auto-continue's
# first-stereo-pair marker. The menu flags can't tell: without stereo the hangar publishes 0x800, the
# same as a load (25 Oct 15:38).
function New-Sm2InGame {
    param([double]$SettleSeconds = 15)
    [pscustomobject]@{ SettleSeconds = $SettleSeconds; PresentSince = $null; Settled = $false }
}
function Step-Sm2InGame {
    param([Parameter(Mandatory)]$State, [string]$NewLog = '', [double]$Elapsed)
    if ($State.Settled) { return $true }
    foreach ($m in [regex]::Matches($NewLog, 'BodyFollowing: enabled=\w+ actor=([0-9a-f]+)')) {
        if ($m.Groups[1].Value -eq '0') { $State.PresentSince = $null }
        elseif ($null -eq $State.PresentSince) { $State.PresentSince = $Elapsed }
    }
    $State.Settled = $null -ne $State.PresentSince -and $Elapsed - $State.PresentSince -ge $State.SettleSeconds
    $State.Settled
}

# After the main menu's Down+Enter: did Continue start? 25 Oct 15:55 it didn't (the menu was still
# coming up and ignored the Down) and the flags sat at 0x800 for nine minutes. 'started' once the
# flags leave 0x800 or the player shows; otherwise every RetrySeconds the next retry: 'enter' (in case
# Continue was selected but not activated), then 'down-enter' (in case nothing was selected: an Enter
# with Continue selected would have started it), then 'give-up'. A Down never follows an Enter that
# worked, so it can't step past Continue onto New game.
function New-Sm2ContinueCheck {
    param([double]$Since, [double]$RetrySeconds = 15)
    [pscustomobject]@{ Since = $Since; RetrySeconds = $RetrySeconds; Retries = 0; Started = $false }
}
function Step-Sm2ContinueCheck {
    param([Parameter(Mandatory)]$State, [string]$NewLog = '', [double]$Elapsed)
    if ($State.Started) { return 'started' }
    $moved = @(Get-Sm2PublishedFlags $NewLog | Where-Object { $_ -ne '0x800' }).Count -gt 0
    $player = [regex]::Matches($NewLog, 'BodyFollowing: enabled=\w+ actor=([0-9a-f]+)') | Where-Object { $_.Groups[1].Value -ne '0' }
    if ($moved -or $player) { $State.Started = $true; return 'started' }
    if ($Elapsed - $State.Since -lt $State.RetrySeconds * ($State.Retries + 1)) { return 'wait' }
    $State.Retries++
    switch ($State.Retries) { 1 { 'enter' } 2 { 'down-enter' } default { 'give-up' } }
}

# The OpenXR session as the mod last reported it (VrSession: status=... every 5 s): 'ready' once the
# runtime has started it, 'waiting' while it waits for readiness (SteamVR in standby: nobody in the
# headset and the proximity sensor uncovered), '' if no status line.
function Get-Sm2VrSession {
    param([string]$NewLog = '')
    $last = ''
    foreach ($m in [regex]::Matches($NewLog, 'VrSession: status=([^\r\n]*)')) {
        $last = if ($m.Groups[1].Value -match 'waiting for session readiness') { 'waiting' } elseif ($m.Groups[1].Value -match 'session state=') { 'ready' } else { $last }
    }
    $last
}

# After leaving a level: 'title' once the title flags show (about 10 s before the title screen is
# drawn; the freeze happens before that), 'hang' once the mod's watchdog fires or the game has both
# stopped logging (the mod's 5 s status lines) and stopped responding for SilentSeconds, else ''.
# The watchdog writes one dump per process, so a stall during startup uses it up (25 Oct 15:44);
# the silence rule still catches the hang then.
function New-Sm2ExitWatch {
    param([double]$Since, [double]$SilentSeconds = 20)
    [pscustomobject]@{ LastLogAt = $Since; SilentSeconds = $SilentSeconds }
}
function Step-Sm2ExitWatch {
    param([Parameter(Mandatory)]$State, [string]$NewLog = '', [double]$Elapsed, [bool]$Responding = $true)
    if ($NewLog -match 'HangWatchdog: no frame presented') { return 'hang' }
    if ((Get-Sm2PublishedFlags $NewLog) -contains $Sm2Flags.Title) { return 'title' }
    if ($NewLog -match 'Sm2Probe: presents=') { $State.LastLogAt = $Elapsed }
    if (-not $Responding -and $Elapsed - $State.LastLogAt -ge $State.SilentSeconds) { return 'hang' }
    ''
}
