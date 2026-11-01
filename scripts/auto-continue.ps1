# -AutoContinue for run-modded.ps1: presses Enter through the startup screens (the mod warnings,
# the title screen, Continue on the current save) until the mod logs its first stereo pair, i.e. the
# player is in the game (the Battle Barge hangar when the save is there). Dot-source this file.
#
# The main menu opens with nothing selected and Continue first, so one Down selects Continue (Enter
# with nothing selected does nothing; two Downs would select New game). The mod logs the native menu
# flags every 5 s ("CinematicState: ... published=0x.."), and 0x800 is the menu bit. 23 Oct runs:
# 0x1400, 0x1800, 0x800, 0x0, then the main menu as 0x1400. Since the 24 Oct runs (and every run on
# 25 Oct): title 0x1400, the "Game files modified" dialog as 0x1800 or 0xc00 (some launches log only
# one of them), then the main menu as 0x800 until something is chosen. So the Down goes when 0x800 has
# followed one of those for five samples in a row (20-25 s: the menu comes up slowly and ignores keys
# until it has; Downs at the second and third samples were ignored at 15:55, 16:17 and 16:25), and
# otherwise when 0x800 has lasted StuckSeconds (the old rule, kept).

# Logged once the first owned stereo pair reaches the headset (Sm2Probe.cpp, "OrdinaryHeadset:").
$Sm2InGameMarker = 'OrdinaryHeadset: consumed='

function New-Sm2AutoContinue {
    param([double]$FirstAt = 15, [double]$Interval = 5, [int]$MaxPresses = 120, [double]$StuckSeconds = 90)
    [pscustomobject]@{ Done = $false; Presses = 0; NextAt = $FirstAt; Interval = $Interval; MaxPresses = $MaxPresses; Offset = 0L
        Flags = ''; FlagsSince = 0.0; FlagsSamples = 0; StuckSeconds = $StuckSeconds; SeenLoad = $false; SeenDialog = $false
        DownSent = $false; InGameAt = $null }
}

# Key schedule from 'ESC@10,DOWN@12': each key at Offset + its seconds. run-modded.ps1 uses it for
# -Keys (Offset 0, from launch) and -InGameKeys (Offset = InGameAt: load times vary, so a menu inside
# the game is only reachable on a clock that starts once the player is in).
function ConvertTo-Sm2KeySchedule {
    param([string]$Keys = '', [double]$Offset = 0, [Parameter(Mandatory)][string[]]$Known)
    foreach ($item in ($Keys -split ',' | Where-Object { $_.Trim() })) {
        $name, $at = $item.Trim() -split '@'
        if ($Known -notcontains $name.ToUpper()) { throw "Unknown key '$name'. Known: $($Known -join ', ')" }
        [pscustomobject]@{ Name = $name.ToUpper(); At = $Offset + [double]$at; Sent = $false }
    }
}

# What to press now: 'none', 'enter', or 'down-enter' (Down, then Enter). Stops for good once the log
# shows the game, or after MaxPresses, so a press can never land in gameplay after stereo has started.
# Down is sent once only: a second one would move the selection past Continue.
function Step-Sm2AutoContinue {
    param([Parameter(Mandatory)]$State, [string]$NewLog = '', [double]$Elapsed)
    if ($State.Done) { return 'none' }
    if ($NewLog.Contains($Sm2InGameMarker)) { $State.Done = $true; $State.InGameAt = $Elapsed; return 'none' }
    foreach ($match in [regex]::Matches($NewLog, 'CinematicState: .*?published=(0x[0-9a-f]+)')) {
        if ($match.Groups[1].Value -ne $State.Flags) { $State.FlagsSince = $Elapsed; $State.FlagsSamples = 0 }
        $State.Flags = $match.Groups[1].Value; $State.FlagsSamples++
        if ($State.Flags -eq '0x800') { $State.SeenLoad = $true }
        if ($State.Flags -in '0x1800', '0xc00') { $State.SeenDialog = $true }   # 16:09 launch: 0xc00 only
    }
    if ($State.Presses -ge $State.MaxPresses) { $State.Done = $true; return 'none' }
    if ($Elapsed -lt $State.NextAt) { return 'none' }
    $State.NextAt = $Elapsed + $State.Interval; $State.Presses++
    $menu = $State.Flags -eq '0x1400' -or ($State.Flags -eq '0x800' -and (($State.SeenDialog -and $State.FlagsSamples -ge 5) -or
        $Elapsed - $State.FlagsSince -ge $State.StuckSeconds))
    if ($State.SeenLoad -and $menu -and -not $State.DownSent) { $State.DownSent = $true; return 'down-enter' }
    return 'enter'
}

# Text appended to the mod log since the last call; the game keeps the file open for writing.
function Read-Sm2LogTail {
    param([Parameter(Mandatory)]$State, [Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    try {
        $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite, Delete')
        try {
            if ($stream.Length -lt $State.Offset) { $State.Offset = 0L }
            [void]$stream.Seek($State.Offset, 'Begin')
            $reader = New-Object IO.StreamReader($stream)
            $text = $reader.ReadToEnd(); $State.Offset = $stream.Position
            return $text
        } finally { $stream.Dispose() }
    } catch { return '' }
}
