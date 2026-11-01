# Checks before install and before each launch. Get-Sm2SystemState reads the machine;
# Test-Sm2Preflight is a pure function of that state, so tests feed it fixture states.

# DLL names other mods use as proxies beside the game exe. The game ships none of these.
$Sm2ProxyNames = @('dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'dinput8.dll', 'version.dll', 'winmm.dll')
# Marker file -> the mod it belongs to, so the message can name it.
$Sm2KnownMods = [ordered]@{ 'ReShade.ini' = 'ReShade'; 'ReShade64.dll' = 'ReShade'; 'dxgi.ini' = 'Special K'; 'SpecialK64.dll' = 'Special K'; 'OptiScaler.ini' = 'OptiScaler'; 'nvngx.ini' = 'OptiScaler' }

function Test-Sm2Writable([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return $false }
    $probe = Join-Path $Directory ('.sm2vr-write-test-' + [guid]::NewGuid().ToString('N'))
    try { [IO.File]::WriteAllText($probe, ''); Remove-Item -LiteralPath $probe -Force; return $true } catch { return $false }
}

function Test-Sm2Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# A non-elevated process cannot read an elevated process's image path; that is the tell.
function Get-Sm2ProcessState([string]$Name) {
    $p = @(Get-Process -Name $Name -ErrorAction SilentlyContinue)
    if (-not $p.Count) { return 'not_running' }
    foreach ($process in $p) { try { if (-not $process.Path) { return 'elevated' } } catch { return 'elevated' } }
    return 'running'
}

# Any XInput pad in slots 0-3. The chase camera is played with a gamepad; motion controls are not.
function Test-Sm2GamepadConnected {
    try {
        if (-not ('Sm2.XInput' -as [type])) {
            Add-Type -Namespace 'Sm2' -Name 'XInput' -MemberDefinition '[DllImport("xinput1_4.dll")] public static extern uint XInputGetState(uint index, byte[] state);'
        }
        foreach ($i in 0..3) { if ([Sm2.XInput]::XInputGetState([uint32]$i, (New-Object byte[] 16)) -eq 0) { return $true } }
    } catch {}
    return $false
}

function Get-Sm2SystemState {
    param($Game, $Version, [string[]]$OwnedNames = @(), [string]$StateRoot)
    $state = [ordered]@{
        GameFound = $null -ne $Game
        GameRoot = $null; ExeTimestamp = $null
        SupportedTimestamp = $Version.game_exe_timestamp; SupportedBuildDate = $Version.game_build_date
        GameRunning = [bool](Get-Process -Name $Sm2ExeName -ErrorAction SilentlyContinue)
        Steam = Get-Sm2ProcessState 'steam'
        SteamVr = Get-Sm2ProcessState 'vrserver'
        InstallerElevated = Test-Sm2Elevated
        OpenXrRuntime = $null
        ProxyFiles = @(); ModMarkers = @(); OwnedLeftovers = @()
        BinWritable = $false; ModsWritable = $false; ModsMissing = $false
        LockPresent = $false
        GamepadConnected = Test-Sm2GamepadConnected
    }
    try { $state.OpenXrRuntime = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction Stop).ActiveRuntime } catch {}
    if ($StateRoot) { $state.LockPresent = Test-Path -LiteralPath (Join-Path $StateRoot 'running.lock') }
    if ($Game) {
        $state.GameRoot = $Game.Root
        try { $state.ExeTimestamp = Get-Sm2PeTimestamp $Game.Exe } catch {}
        foreach ($name in $Sm2ProxyNames) {
            if (Test-Path -LiteralPath (Join-Path $Game.Bin $name)) {
                if ($OwnedNames -contains $name) { $state.OwnedLeftovers += $name } else { $state.ProxyFiles += $name }
            }
        }
        foreach ($marker in $Sm2KnownMods.Keys) {
            if (Test-Path -LiteralPath (Join-Path $Game.Bin $marker)) { $state.ModMarkers += $Sm2KnownMods[$marker] }
        }
        $state.ModMarkers = @($state.ModMarkers | Select-Object -Unique)
        $state.BinWritable = Test-Sm2Writable $Game.Bin
        $state.ModsMissing = -not (Test-Path -LiteralPath $Game.Mods -PathType Container)
        $state.ModsWritable = $(if ($state.ModsMissing) { Test-Sm2Writable $Game.ClientRoot } else { Test-Sm2Writable $Game.Mods })
    }
    return [pscustomobject]$state
}

function New-Sm2Check([string]$Id, [bool]$Ok, [string]$Message, [string]$Fix = '', [bool]$Blocking = $true) {
    [pscustomobject]@{ Id = $Id; Ok = $Ok; Blocking = $Blocking; Message = $Message; Fix = $Fix }
}

# -For 'install' or 'play'. Play additionally needs Steam running and no game already open.
function Test-Sm2Preflight {
    param([Parameter(Mandatory)]$State, [ValidateSet('install', 'play')][string]$For = 'install')
    $checks = New-Object System.Collections.Generic.List[object]
    if (-not $State.GameFound) {
        $checks.Add((New-Sm2Check 'game' $false 'Space Marine 2 was not found in your Steam libraries.' 'Install it through Steam, or press Change and pick the game folder.'))
        return $checks
    }
    $checks.Add((New-Sm2Check 'game' $true "Game found: $($State.GameRoot)"))
    if (-not $State.ExeTimestamp) {
        $checks.Add((New-Sm2Check 'version' $false 'The game exe could not be read.' 'Verify the game files in Steam (Properties > Installed Files > Verify).'))
    } elseif ($State.ExeTimestamp -ne $State.SupportedTimestamp) {
        $checks.Add((New-Sm2Check 'version' $false "This game version ($($State.ExeTimestamp)) is not the one this mod release supports ($($State.SupportedTimestamp), $($State.SupportedBuildDate))." 'Space Marine 2 has probably updated. Wait for a mod update, then press Check for updates.'))
    } else {
        $checks.Add((New-Sm2Check 'version' $true "Game version supported ($($State.SupportedBuildDate))."))
    }
    if ($State.InstallerElevated) {
        $checks.Add((New-Sm2Check 'elevation' $false 'SM2VR is running as administrator.' 'Close it and start SM2VR.cmd normally (not "Run as administrator"). SteamVR cannot talk to an elevated game.'))
    }
    if ($State.Steam -eq 'elevated') {
        $checks.Add((New-Sm2Check 'steam' $false 'Steam is running as administrator.' 'Quit Steam and start it normally.'))
    } elseif ($For -eq 'play' -and $State.Steam -ne 'running') {
        $checks.Add((New-Sm2Check 'steam' $false 'Steam is not running.' 'Start Steam and sign in; the game needs it.'))
    }
    if ($State.SteamVr -eq 'elevated') {
        $checks.Add((New-Sm2Check 'steamvr' $false 'SteamVR is running as administrator.' 'Quit SteamVR and start it normally.'))
    }
    if (-not $State.OpenXrRuntime -or $State.OpenXrRuntime -notmatch 'steamxr') {
        $current = $(if ($State.OpenXrRuntime) { $State.OpenXrRuntime } else { 'none' })
        $checks.Add((New-Sm2Check 'openxr' $false "SteamVR is not the active OpenXR runtime (current: $current)." 'Open SteamVR > Settings > OpenXR and press "Set SteamVR as OpenXR runtime".'))
    }
    if (@($State.ProxyFiles).Count) {
        $who = $(if (@($State.ModMarkers).Count) { ' (looks like ' + ($State.ModMarkers -join ', ') + ')' } else { '' })
        $checks.Add((New-Sm2Check 'other_mods' $false ("Another mod's files are beside the game exe: " + ($State.ProxyFiles -join ', ') + $who + '.') 'Remove or disable that mod first; SM2VR needs dxgi.dll and never overwrites another mod.'))
    }
    if (@($State.OwnedLeftovers).Count -or $State.LockPresent) {
        $checks.Add((New-Sm2Check 'leftovers' $false 'The last VR session did not end cleanly.' 'SM2VR will remove its leftover files now.' $false))
    }
    if (-not $State.BinWritable -or -not $State.ModsWritable) {
        $checks.Add((New-Sm2Check 'writable' $false 'The game folder is not writable for your Windows user.' 'Press Fix permissions (one administrator prompt), or move the game to a library outside Program Files.'))
    }
    if ($For -eq 'play' -and -not $State.GamepadConnected) {
        $checks.Add((New-Sm2Check 'gamepad' $false 'No gamepad is connected.' 'The chase camera is played with a gamepad: connect an Xbox-compatible controller before you press Play.' $false))
    }
    if ($For -eq 'play' -and $State.GameRunning) {
        $checks.Add((New-Sm2Check 'running' $false 'Space Marine 2 is already running.' 'Close the game first.'))
    }
    return $checks
}

function Test-Sm2PreflightPassed($Checks) {
    return -not @($Checks | Where-Object { -not $_.Ok -and $_.Blocking }).Count
}
