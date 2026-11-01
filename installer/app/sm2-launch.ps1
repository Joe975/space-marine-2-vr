# Play: copy the mod into the game folder, start the game without EAC, wait, remove exactly what
# was copied. A crash between those steps is repaired on the next start (running.lock).
# Same steps as scripts/run-modded.ps1 minus its test-only parts (keys, screenshots, RenderDoc).

# In a release the shared dev helpers ship in app\lib; in the repo they live in scripts\.
$Sm2Lib = $(if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'lib')) { Join-Path $PSScriptRoot 'lib' } else { Join-Path $PSScriptRoot '..\..\scripts' })
. (Join-Path $Sm2Lib 'owned-game-process.ps1')

# Files the framework core drops beside the exe (run-modded.ps1 $droppings).
$Sm2Droppings = 'vr_config.txt', 'vr_ui.ini', 'ref_ui.ini', 'vr_log.txt', 'vrframework_crash.dmp', 'imgui.ini'
$Sm2PakName = 'SM2VR_FirstPerson_Test.pak'

function Get-Sm2LogRoot { Join-Path $env:LOCALAPPDATA 'sm2vr' }

# Log line -> startup phase shown to the player. Lines come from mod/src (OpenXrBackend.cpp,
# OpenVrBackend.cpp, GameBuild) and the framework core; keep them in step with those sources.
$Sm2Phases = @(
    @{ Id = 'loaded'; Pattern = 'sm2vr loaded as dxgi proxy'; Text = 'Mod loaded into the game.' },
    @{ Id = 'unknown_build'; Pattern = 'GameBuild: .*known=false'; Text = 'The mod does not know this game version; VR may not start.' },
    @{ Id = 'hooked'; Pattern = 'Framework initialized'; Text = 'Graphics hooks ready.' },
    @{ Id = 'window'; Pattern = 'Sm2Probe: first present'; Text = 'Game window open. The first start after a game update compiles shaders and can take a few minutes.' },
    @{ Id = 'headset_wait'; Pattern = 'waiting for session readiness'; Text = 'Waiting for the headset: put it on.' },
    @{ Id = 'vr'; Pattern = '(OpenXR|OpenVR) tracking active'; Text = 'In VR.' },
    # Sm2Probe.cpp: a swap-chain reset (the game changing display mode) stops stereo and re-arms it.
    @{ Id = 'stereo_restarted'; Pattern = 'OrdinaryHeadsetRearm: armed=true'; Text = 'The game changed display mode; VR is restarting.' },
    # Any other stop restarts the whole stereo stack a few seconds later (OrdinaryColdRestart), up to
    # a budget; only the budget running out, or a failed re-arm, ends VR for the run.
    @{ Id = 'stereo_cold_restart'; Pattern = 'OrdinaryHeadsetColdRestart:'; Text = 'VR hit a problem and restarted itself.' },
    @{ Id = 'stereo_stopped'; Pattern = 'OrdinaryHeadsetStopped: budget=exhausted|OrdinaryHeadsetRearm: armed=false'; Text = 'VR stopped for this run. Close the game and play again.' },
    # Sm2Probe.cpp: stereo needs the game's DLSS or TAA. A player on FSR or MFSR gets the flat game in
    # the headset, on every launch until the setting changes, so say which setting it is.
    @{ Id = 'stereo_needs_dlss'; Pattern = 'OrdinaryDlssHold: held=true'; Text = 'VR needs DLSS or TAA. The headset shows the flat game until you pick one in the game''s video settings. You do not need to restart.' },
    @{ Id = 'stereo_dlss_back'; Pattern = 'OrdinaryDlssHold: held=false'; Text = 'DLSS or TAA is on; VR is returning.' }
)

# Watches the log a batch at a time. Each line is matched once, against the phases not reached yet,
# so a poll costs only the lines appended since the last one, however long the game has run.
# Examined counts the lines matched; Done is true once every phase has been reached.
function New-Sm2PhaseWatch {
    $pending = New-Object System.Collections.Generic.List[object]
    foreach ($phase in $Sm2Phases) { $pending.Add($phase) }
    [pscustomobject]@{ Pending = $pending; Examined = 0; Done = $false }
}

# Phases first reached in this batch of new lines, in $Sm2Phases order, each once per watch.
function Add-Sm2PhaseLines($Watch, [string[]]$Lines) {
    $reached = New-Object System.Collections.Generic.List[object]
    if ($Watch.Done -or -not $Lines) { return $reached }
    $Watch.Examined += $Lines.Count
    foreach ($phase in $Watch.Pending.ToArray()) {
        foreach ($line in $Lines) {
            if ($line -match $phase.Pattern) { $reached.Add([pscustomobject]@{ Id = $phase.Id; Text = $phase.Text }); [void]$Watch.Pending.Remove($phase); break }
        }
    }
    $Watch.Done = $Watch.Pending.Count -eq 0
    return $reached
}

# Phases reached by these log lines, in order, each once.
function Get-Sm2StartupPhases([string[]]$Lines) { Add-Sm2PhaseLines (New-Sm2PhaseWatch) $Lines }

# One poll of the running game's log: reads the lines appended since -Offset and returns the phases
# they reach. Stops reading the log once every phase has been reached.
function Update-Sm2PhaseWatch {
    param($Watch, [string]$Path, [ref]$Offset)
    if ($Watch.Done) { return @() }
    Add-Sm2PhaseLines $Watch @(Read-Sm2NewLines -Path $Path -Offset $Offset)
}

# Reads what was appended to a file since the last call (the game keeps it open for writing).
function Read-Sm2NewLines {
    param([string]$Path, [ref]$Offset)
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($stream.Length -lt $Offset.Value) { $Offset.Value = 0 }
        $stream.Position = $Offset.Value
        $reader = New-Object IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
        $Offset.Value = $stream.Position
    } finally { $stream.Dispose() }
    return @($text -split "`r?`n" | Where-Object { $_ })
}

# Copies one file into the game folder. Recorded in install.json before the copy, so a crash at any
# point leaves a record. Refuses to replace a file the mod does not own.
function Add-Sm2GameFile {
    param([string]$StateRoot, [string]$Source, [string]$Target)
    if (Test-Path -LiteralPath $Target) { throw "Refusing to replace an existing file: $Target" }
    $hash = Get-Sm2Sha256 $Source
    $state = Read-Sm2InstallState $StateRoot
    $state.game_files = @(@($state.game_files) + [pscustomobject]@{ path = $Target; sha256 = $hash })
    Write-Sm2InstallState $StateRoot $state
    Copy-Item -LiteralPath $Source -Destination $Target
}

# Removes every recorded game file whose content is still the mod's. A changed file is left in
# place and reported. Returns the warnings.
function Remove-Sm2GameFiles([string]$StateRoot) {
    $state = Read-Sm2InstallState $StateRoot
    if (-not $state) { return @() }
    $warnings = @(); $left = @()
    foreach ($file in @($state.game_files)) {
        if (-not $file -or -not (Test-Path -LiteralPath $file.path)) { continue }
        if ((Get-Sm2Sha256 $file.path) -ne $file.sha256) { $warnings += "Changed since the mod copied it, left in place: $($file.path)"; continue }
        $removed = $false
        for ($i = 0; $i -lt 10 -and -not $removed; $i++) {
            try { Remove-Item -LiteralPath $file.path -Force -ErrorAction Stop; $removed = $true } catch { Start-Sleep -Seconds 1 }
        }
        if (-not $removed) { $warnings += "Could not remove (still in use): $($file.path)"; $left += $file }
    }
    $state.game_files = @($left)
    Write-Sm2InstallState $StateRoot $state
    return $warnings
}

function Move-Sm2RunFiles {
    param([string]$Bin, [string]$LogRoot, [string]$RunDir)
    New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
    foreach ($f in $Sm2Droppings) {
        $src = Join-Path $Bin $f
        if (Test-Path -LiteralPath $src) { Move-Item -LiteralPath $src -Destination (Join-Path $RunDir $f) -Force }
    }
    foreach ($filter in 'sm2vr*.log', 'sm2vr_trace*.jsonl') {
        Get-ChildItem -LiteralPath $LogRoot -Filter $filter -File -ErrorAction SilentlyContinue | ForEach-Object {
            try { Move-Item -LiteralPath $_.FullName -Destination $RunDir -Force } catch {}
        }
    }
}

function Get-Sm2BinProcesses([string]$Bin) {
    @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Bin, [StringComparison]::OrdinalIgnoreCase) })
}

# After a crash (lock left, or recorded files still in the game folder) with no game running.
# Returns $null when there was nothing to repair.
function Repair-Sm2AfterCrash {
    param([Parameter(Mandatory)]$Game, [string]$StateRoot = (Get-Sm2StateRoot), [string]$LogRoot = (Get-Sm2LogRoot))
    $lock = Join-Path $StateRoot 'running.lock'
    $state = Read-Sm2InstallState $StateRoot
    $files = @(); if ($state) { $files = @(@($state.game_files) | Where-Object { $_ }) }
    if (-not (Test-Path -LiteralPath $lock) -and -not $files.Count) { return $null }
    if (Get-Sm2BinProcesses $Game.Bin) { throw 'The game is still running; close it first.' }
    $runDir = Join-Path $StateRoot ('runs\' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '_recovered')
    $warnings = @(Remove-Sm2GameFiles $StateRoot)
    Move-Sm2RunFiles -Bin $Game.Bin -LogRoot $LogRoot -RunDir $runDir
    if (Test-Path -LiteralPath $lock) { Move-Item -LiteralPath $lock -Destination (Join-Path $runDir 'running.lock') -Force }
    return [pscustomobject]@{ RunDir = $runDir; Warnings = $warnings }
}

# One play session. Blocks until the game exits. -OnPhase gets each startup phase as reached.
# -View motion: first person with the Index controllers. -View chase: the game's own chase camera
# with a gamepad, from the chase pack (docs/THIRD_PERSON.md).
function Start-Sm2Play {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)]$Game, [string]$StateRoot = (Get-Sm2StateRoot),
        [string]$LogRoot = (Get-Sm2LogRoot), [string[]]$GameArguments = @(), [scriptblock]$OnPhase = { param($phase) },
        [int]$PollMilliseconds = 500, [ValidateSet('motion', 'chase')][string]$View = 'motion')
    $problems = @(Get-Sm2InstallProblems -InstallState (Read-Sm2InstallState $StateRoot) -Package $Package -Game $Game)
    if ($problems.Count) { throw ('Install first: ' + ($problems -join '; ')) }
    if (Get-Sm2BinProcesses $Game.Bin) { throw 'Space Marine 2 is already running.' }
    Repair-Sm2AfterCrash -Game $Game -StateRoot $StateRoot -LogRoot $LogRoot | Out-Null
    $install = Read-Sm2InstallState $StateRoot
    $playProfile = Read-Sm2Profile $Package.Profile
    $runDir = Join-Path $StateRoot ('runs\' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '_play')
    $launch = ConvertTo-Sm2Launch -Profile $playProfile -ConfigDirectory (Join-Path $StateRoot 'config') -DumpDirectory (Join-Path $runDir 'dump') -Chase:($View -eq 'chase')
    $pack = $(if ($View -eq 'chase') { $install.pack.chase.path } else { $install.pack.path })
    if ($launch.Env.Contains('SM2VR_GRENADE_NATIVE_PROBE')) {
        if ($install.grenade_layout_verified) { $launch.Env['SM2VR_GRENADE_EQUIPMENT_LAYOUT'] = '192' }
        else { foreach ($n in 'SM2VR_GRENADE_NATIVE_PROBE', 'SM2VR_GRENADE_AIM', 'SM2VR_GRENADE_AIM_CONFIG') { $launch.Env.Remove($n) } }
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $runDir 'dump') | Out-Null
    foreach ($path in $launch.Files.Keys) { [IO.File]::WriteAllText($path, $launch.Files[$path], (New-Object Text.ASCIIEncoding)) }
    $lock = Join-Path $StateRoot 'running.lock'
    Write-Sm2Json $lock @{ started_utc = (Get-Date).ToUniversalTime().ToString('o'); run_dir = $runDir; pid = $null }
    New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
    Remove-Item (Join-Path $LogRoot 'sm2vr*.log'), (Join-Path $LogRoot 'sm2vr_trace*.jsonl') -ErrorAction SilentlyContinue
    $saved = @{}; Get-ChildItem Env:SM2VR_* | ForEach-Object { $saved[$_.Name] = $_.Value }
    $result = [pscustomobject]@{ RunDir = $runDir; ExitCode = $null; Phases = @(); Warnings = @() }
    $process = $null
    try {
        if (-not (Test-Path -LiteralPath $Game.Mods)) { New-Item -ItemType Directory -Path $Game.Mods | Out-Null }
        Add-Sm2GameFile $StateRoot $Package.Dll (Join-Path $Game.Bin 'dxgi.dll')
        Add-Sm2GameFile $StateRoot $pack (Join-Path $Game.Mods $Sm2PakName)
        if ($playProfile.runtime -eq 'OpenVR') {
            $runtime = Join-Path $Game.Bin 'openvr_api.dll'
            # An identical copy someone else put there is borrowed, not owned.
            if (-not (Test-Path -LiteralPath $runtime) -or (Get-Sm2Sha256 $runtime) -ne (Get-Sm2Sha256 $Package.OpenVrDll)) {
                Add-Sm2GameFile $StateRoot $Package.OpenVrDll $runtime
            }
        }
        Get-ChildItem Env:SM2VR_* | Remove-Item
        foreach ($name in $launch.Env.Keys) { Set-Item -LiteralPath ('Env:' + $name) -Value $launch.Env[$name] }
        $process = Start-Sm2OwnedGame -Executable $Game.Exe -ArgumentList $GameArguments
        Write-Sm2Json $lock @{ started_utc = (Get-Date).ToUniversalTime().ToString('o'); run_dir = $runDir; pid = $process.Id }
        $offset = 0; $watch = New-Sm2PhaseWatch
        $log = Join-Path $LogRoot 'sm2vr.log'
        while ($true) {
            $alive = $null -ne (Get-Sm2OwnedGame $process)
            foreach ($phase in (Update-Sm2PhaseWatch -Watch $watch -Path $log -Offset ([ref]$offset))) {
                $result.Phases += $phase.Id; & $OnPhase $phase
            }
            if (-not $alive) { break }
            Start-Sleep -Milliseconds $PollMilliseconds
        }
        try { $result.ExitCode = $process.ExitCode } catch {}
    } finally {
        Get-ChildItem Env:SM2VR_* | Remove-Item
        foreach ($name in $saved.Keys) { Set-Item -LiteralPath ('Env:' + $name) -Value $saved[$name] }
        # Helper processes outlive the game window for a few seconds and keep the DLL locked.
        for ($i = 0; $i -lt 30 -and (Get-Sm2BinProcesses $Game.Bin).Count; $i++) { Start-Sleep -Seconds 1 }
        if ((Get-Sm2BinProcesses $Game.Bin).Count) {
            $result.Warnings += 'The game is still running; its mod files stay until SM2VR starts again after it closes.'
        } else {
            $result.Warnings += @(Remove-Sm2GameFiles $StateRoot)
            Move-Sm2RunFiles -Bin $Game.Bin -LogRoot $LogRoot -RunDir $runDir
            if (-not @((Read-Sm2InstallState $StateRoot).game_files | Where-Object { $_ }).Count) { Remove-Item -LiteralPath $lock -Force -ErrorAction SilentlyContinue }
        }
        if ($process) { $process.Dispose() }
    }
    return $result
}

# Removes everything the mod put in the game folder and its own install; keeps save backups and
# run logs (they are the player's).
function Uninstall-Sm2 {
    param([Parameter(Mandatory)]$Game, [string]$StateRoot = (Get-Sm2StateRoot), [string]$LogRoot = (Get-Sm2LogRoot))
    if (Get-Sm2BinProcesses $Game.Bin) { throw 'Close the game first.' }
    $repair = Repair-Sm2AfterCrash -Game $Game -StateRoot $StateRoot -LogRoot $LogRoot
    $warnings = @(); if ($repair) { $warnings = @($repair.Warnings) }
    $state = Read-Sm2InstallState $StateRoot
    if ($state -and @(@($state.game_files) | Where-Object { $_ }).Count) { throw ('Some mod files could not be removed: ' + ($warnings -join '; ')) }
    foreach ($item in 'pack', 'config', 'config-shipped', 'install.json') {
        $path = Join-Path $StateRoot $item
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    }
    return $warnings
}
