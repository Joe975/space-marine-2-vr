# Test run of the mod: installs build\mod\bin\dxgi.dll next to the game exe, starts the game without
# Easy Anti-Cheat, waits, screenshots the game window, closes the game, and ALWAYS removes the DLL
# again so that a normal (EAC) launch from Steam never sees a modified folder.
#
# Usage: run-modded.ps1 [-Seconds 90] [-Tag name] [-NoClose] [-Keys 'F11@50,ENTER@55']
#   -NoClose leaves the game running (the DLL then stays installed until you run uninstall-mod.ps1).
#   -Keys presses keys at the given number of seconds after launch (F11 toggles the mod overlay, closed at start; ENTER
#    leaves the title screen). A key is only sent while the game window is the foreground window, so
#    it can never land in another application.
#   -Shots '50,80' also screenshots the game window at those seconds (window_50.png, ...).
#   -AutoContinue presses Enter every 5 s from 15 s (same foreground rule) until the mod logs its first
#    stereo pair: through the mod warnings and title screen into the current save (scripts/auto-continue.ps1).
#   -InGameKeys 'ESC@10,DOWN@12' (needs -AutoContinue) is like -Keys, but its seconds count from that first
#    stereo pair, so a scripted menu sequence inside the game doesn't depend on how long the load took.
#   Environment variables starting with SM2VR_ set before calling this script reach the mod.
#   -LaunchFlags (run-ordinary-headset.ps1 passes it) is printed as "launch flags: ..." and saved as
#    launch-flags.json in the run folder.
#   -GameArgs '--startup --set /Config/...' is passed to the game exe (see docs/DIRECT_LAUNCH.md).
#   -Resolution 2560x1440 -WindowMode Windowed|Borderless|Fullscreen -RenderResolution Native|Quality|
#    Balanced|Performance|UltraPerformance|Dynamic override the game's saved video settings for this
#    launch only; the original settings files are restored byte-for-byte on exit (docs/RESOLUTION.md).
#   -Antialiasing TAA|FSR2|DLSS|MFSR does the same for the game's anti-aliasing method.
param([int]$Seconds = 90, [string]$Tag = 'run', [switch]$NoClose, [string]$Keys = '', [string]$Shots = '', [string]$DataPak = '', [switch]$NoUiAutomation, [string]$GameArgs = '', [switch]$RenderDocCapture, [switch]$AutoContinue, [string]$InGameKeys = '',
    [string]$Resolution = '', [string]$WindowMode = '', [string]$RenderResolution = '', [string]$OutputSize = '', [string]$Antialiasing = '',
    [System.Collections.IDictionary]$LaunchFlags = $null)

# Only simple unquoted engine option tokens are supported by the direct launcher.
if ($GameArgs -match '[^a-zA-Z0-9_./= :\-]') { throw '-GameArgs supports only simple engine option tokens.' }
$gameArgList = @($GameArgs -split ' +' | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'game-video-settings.ps1')
# The mod's output-size override must always run with the settings-file transaction.
if ([string]$env:SM2VR_OUTPUT_SIZE -cne $OutputSize) { throw '-OutputSize and SM2VR_OUTPUT_SIZE must be set together, to the same value.' }
$videoOverride = New-Sm2VideoOverride -Resolution $Resolution -WindowMode $WindowMode -RenderResolution $RenderResolution -OutputSize $OutputSize -Antialiasing $Antialiasing
# A game left running keeps the overridden settings and rewrites them on exit; never allow that.
if ($videoOverride -and $NoClose) { throw '-NoClose cannot be combined with -Resolution, -WindowMode, -RenderResolution, -Antialiasing or -OutputSize.' }
$videoReceipt = $null; $videoRestored = $null

# Computer Use owns all UI input/screenshots in this mode; the launcher only manages its process/files.
if ($NoUiAutomation -and ($Keys -or $Shots -or $AutoContinue)) { throw '-NoUiAutomation cannot be combined with -Keys, -Shots or -AutoContinue.' }
if ($InGameKeys -and -not $AutoContinue) { throw '-InGameKeys needs -AutoContinue (its clock starts at the first stereo pair).' }
. (Join-Path $PSScriptRoot 'auto-continue.ps1')
$continueState = if ($AutoContinue) { New-Sm2AutoContinue } else { $null }
$continueActivated = $false
if ($env:SM2VR_ORDINARY_OBSERVER -eq '1') {
    $ordinaryCaptureDirectory = $env:SM2VR_ORDINARY_DUMP_DIR
    if (-not $ordinaryCaptureDirectory -or -not [System.IO.Path]::IsPathRooted($ordinaryCaptureDirectory) -or
        -not (Test-Path -LiteralPath $ordinaryCaptureDirectory -PathType Container)) {
        throw 'Ordinary observer requires an absolute existing SM2VR_ORDINARY_DUMP_DIR before launch.'
    }
}

$ErrorActionPreference = 'Stop'
if ($env:SM2VR_OPENXR -eq '1' -and ($env:SM2VR_OPENVR -eq '1' -or $env:SM2VR_SYNTHETIC_EYES -eq '1')) {
    throw 'OpenXR requires OpenVR and synthetic eyes disabled.'
}
$root    = Split-Path $PSScriptRoot -Parent
# Found through Steam (installer/app/sm2-game.ps1); SM2VR_GAME_ROOT overrides.
. (Join-Path $root 'installer\app\sm2-game.ps1')
$game = Find-Sm2Game
if (-not $game) { throw 'Space Marine 2 was not found through Steam; set SM2VR_GAME_ROOT to the game folder.' }
$gameBin = $game.Bin
$exeName = 'Warhammer 40000 Space Marine 2 - Retail'
$modDll  = Join-Path $root 'build\mod\bin\dxgi.dll'
$target  = Join-Path $gameBin 'dxgi.dll'
$logDir  = Join-Path $env:LOCALAPPDATA 'sm2vr'
$runDir  = Join-Path $root ("build\run\{0}_{1}" -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $Tag)
$captureTool = Join-Path $root 'build\tools\renderdoc-1.46\RenderDoc_1.46_64\renderdoccmd.exe'
if ($RenderDocCapture -and -not (Test-Path -LiteralPath $captureTool -PathType Leaf)) { throw "Portable RenderDoc is missing: $captureTool" }
# Files the framework core drops next to the exe.
$droppings = 'vr_config.txt', 'vr_ui.ini', 'ref_ui.ini', 'vr_log.txt', 'vrframework_crash.dmp', 'imgui.ini'
. (Join-Path $PSScriptRoot 'test-data-pak.ps1')
$dataPakReceipt = $null
. (Join-Path $PSScriptRoot 'runtime-api.ps1')
$runtimeApiReceipt = $null

. (Join-Path $PSScriptRoot 'owned-game-process.ps1')
$ownedGame = $null
$captureHelper = $null
function Get-ExistingGames {
    Get-Process -Name $exeName -ErrorAction SilentlyContinue | Where-Object {
        # Fresh enumerations may still list a terminated process while a handle
        # exists. Unknown exit state remains conservative; a numeric ID is not proof.
        try { -not $_.HasExited } catch { $true }
    }
}
# Every process started from the game's bin folder loads the proxy, not only the game window's process.
function Get-BinProcesses { Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($gameBin, [StringComparison]::OrdinalIgnoreCase) } }

# Sm2Keys, Send-Sm2Key and the scan-code table (scripts/game-keys.ps1).
. (Join-Path $PSScriptRoot 'game-keys.ps1') # defines only; nothing is sent under -NoUiAutomation
$scanCodes = $Sm2ScanCodes
$schedule = @(ConvertTo-Sm2KeySchedule -Keys $Keys -Known @($scanCodes.Keys))
# Checked now, scheduled once the player is in (InGameAt).
$null = @(ConvertTo-Sm2KeySchedule -Keys $InGameKeys -Known @($scanCodes.Keys)); $inGameScheduled = -not $InGameKeys

$shotTimes = @($Shots -split ',' | Where-Object { $_ } | ForEach-Object { [pscustomobject]@{ At = [double]$_; Done = $false } })

if (Get-ExistingGames) { throw 'The game is already running.' }
if (-not (Test-Path $modDll)) { throw "Build the mod first: $modDll is missing." }
if (Test-Path $target) { throw "$target already exists; run uninstall-mod.ps1 first." }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running.' }
# Every launch refuses while an earlier override is unrestored, with or without new overrides.
Assert-Sm2NoPendingVideoSettings

New-Item -ItemType Directory -Force $runDir | Out-Null
# -LaunchFlags (from run-ordinary-headset.ps1): what this launch runs beyond the player's defaults (docs/FLAGS.md).
if ($LaunchFlags) {
    "launch flags: $($LaunchFlags.summary)"
    [IO.File]::WriteAllText((Join-Path $runDir 'launch-flags.json'), ($LaunchFlags | ConvertTo-Json -Depth 4))
}
Remove-Item (Join-Path $logDir 'sm2vr*.log') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $logDir 'sm2vr_trace*.jsonl') -ErrorAction SilentlyContinue
$closed = $false
try {
    if ($env:SM2VR_GRENADE_NATIVE_PROBE -eq '1') {
        # A git worktree has no .venv of its own; use the main checkout's.
        $python = Join-Path $root '.venv\Scripts\python.exe'
        $layoutVerified = $false
        try {
            if (-not (Test-Path -LiteralPath $python)) {
                $common = git -C $root rev-parse --path-format=absolute --git-common-dir
                if ($common) { $python = Join-Path (Split-Path $common -Parent) '.venv\Scripts\python.exe' }
            }
            $grenadeLayout = & $python -I (Join-Path $PSScriptRoot 'inspect_grenade_equipment_layout.py') (Join-Path (Split-Path (Split-Path $gameBin -Parent) -Parent) 'paks\client\default\default_ssl.pak')
            $layoutVerified = $LASTEXITCODE -eq 0
        } catch { $layoutVerified = $false }
        if ($layoutVerified) {
            $grenadeLayout | Set-Content -LiteralPath (Join-Path $runDir 'grenade-equipment-layout.json')
            $env:SM2VR_GRENADE_EQUIPMENT_LAYOUT='192'
        } else {
            # Grenade aim is on by default, so an unknown layout (game patch) must not block the launch.
            # Without the verified layout nothing is hooked and grenades keep the game's camera aim.
            Write-Warning 'Grenade aim off: the installed grenade equipment ownership layout is unsupported.'
            'unsupported' | Set-Content -LiteralPath (Join-Path $runDir 'grenade-equipment-layout.json')
            Remove-Item Env:SM2VR_GRENADE_NATIVE_PROBE, Env:SM2VR_GRENADE_AIM, Env:SM2VR_GRENADE_AIM_CANDIDATE -ErrorAction SilentlyContinue
        }
    }
    if ($env:SM2VR_OPENVR -eq '1' -and $env:SM2VR_SYNTHETIC_EYES -ne '1') {
        $runtimeApiReceipt = Install-Sm2RuntimeApi (Join-Path $root 'refs\vrframework-core\shipping\openvr_api.dll') (Join-Path $gameBin 'openvr_api.dll')
        $runtimeApiReceipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'runtime-api.json')
        "runtime api ready: $($runtimeApiReceipt.Path)"
    }
    if ($DataPak) {
        $dataPakReceipt = Install-Sm2TestDataPak $DataPak (Split-Path (Split-Path $gameBin -Parent) -Parent) $runDir
        "installed data pack $($dataPakReceipt.Path)"
    }
    if ($videoOverride) {
        $videoReceipt = Install-Sm2VideoSettings $videoOverride (Join-Path $env:LOCALAPPDATA 'Saber\Space Marine 2\storage') $runDir
        "video settings override: $(($videoOverride.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ') (originals backed up in $runDir\video-settings-backup)"
    }
    Copy-Item $modDll $target
    "installed $target"
    if ($GameArgs) { "game args: $GameArgs"; $GameArgs | Set-Content -LiteralPath (Join-Path $runDir 'game-args.txt') }
    $start = Get-Date
    $launchOptions=@{Executable=(Join-Path $gameBin ($exeName+'.exe'));ArgumentList=$gameArgList}
    if ($RenderDocCapture) { $launchOptions.CaptureTool=$captureTool;$launchOptions.CapturePrefix=Join-Path $runDir 'renderdoc_frame';$launchOptions.CaptureHelper=[ref]$captureHelper }
    $ownedGame = Start-Sm2OwnedGame @launchOptions
    "owned game: pid=$($ownedGame.Id) created=$($ownedGame.StartTime.ToUniversalTime().ToString('o'))"
    @{pid=$ownedGame.Id;created_utc=$ownedGame.StartTime.ToUniversalTime().ToString('o');executable=(Join-Path $gameBin ($exeName+'.exe'));arguments=$gameArgList} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'owned-process.json')
    $deadline = $start.AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $p = Get-Sm2OwnedGame $ownedGame
        if (-not $p) { 'game exited on its own'; break }
        $t = ((Get-Date) - $start).TotalSeconds
        foreach ($k in $schedule | Where-Object { -not $_.Sent -and $t -ge $_.At }) {
            $k.Sent = $true
            $ok = $p -and [Sm2Keys]::Press([uint16]$scanCodes[$k.Name], [uint32]$p.Id, ($k.Name -in 'UP','DOWN','LEFT','RIGHT'))
            "t+$([int]$t)s key $($k.Name): $(if ($ok) { 'sent' } else { 'skipped (game window not in foreground)' })"
        }
        if ($continueState -and -not $continueState.Done) {
            $press = Step-Sm2AutoContinue $continueState (Read-Sm2LogTail $continueState (Join-Path $logDir 'sm2vr.log')) $t
            if ($press -ne 'none') {
                # A game started from a background process may never get the foreground (Windows'
                # foreground lock), and auto-continue only presses into the game. Bring it forward once.
                if (-not $continueActivated -and [Sm2Keys]::ForegroundPid() -ne [uint32]$p.Id) {
                    $continueActivated = $true; $p.Refresh()
                    "t+$([int]$t)s auto-continue: game not in foreground; activate=$([Sm2Keys]::Activate($p.MainWindowHandle))"
                    Start-Sleep -Milliseconds 500
                }
                $ok = $true
                if ($press -eq 'down-enter') {
                    $ok = [Sm2Keys]::Press([uint16]$scanCodes.DOWN, [uint32]$p.Id, $true); Start-Sleep -Milliseconds 400
                    if (-not $ok) { $continueState.DownSent = $false } # Not in front: Down again next time.
                }
                $ok = $ok -and [Sm2Keys]::Press([uint16]$scanCodes.ENTER, [uint32]$p.Id)
                "t+$([int]$t)s auto-continue $($press.ToUpper()) $($continueState.Presses) (menu flags $($continueState.Flags)): $(if ($ok) { 'sent' } else { 'skipped (game window not in foreground)' })"
            } elseif ($continueState.Done) { "t+$([int]$t)s auto-continue done after $($continueState.Presses) presses" }
        }
        if (-not $inGameScheduled -and $continueState.InGameAt -ne $null) {
            $inGameScheduled = $true
            $schedule += @(ConvertTo-Sm2KeySchedule -Keys $InGameKeys -Offset $continueState.InGameAt -Known @($scanCodes.Keys))
            "t+$([int]$t)s in game: scheduled $InGameKeys from t+$([int]$continueState.InGameAt)s"
        }
        foreach ($s in $shotTimes | Where-Object { -not $_.Done -and $t -ge $_.At }) {
            $s.Done = $true
            if ($p) { try { & (Join-Path $PSScriptRoot 'screenshot.ps1') -Out (Join-Path $runDir "window_$([int]$s.At).png") -ProcessId $p.Id } catch { "screenshot failed: $_" } }
        }
    }
    $p = Get-Sm2OwnedGame $ownedGame
    if ($p) {
        "running: ws=$([int]($p.WorkingSet64 / 1MB))MB title='$($p.MainWindowTitle)' responding=$($p.Responding)"
        Get-BinProcesses | ForEach-Object { "pid=$($_.ProcessId) parent=$($_.ParentProcessId) ws=$([int]($_.WorkingSetSize / 1MB))MB cmd=$($_.CommandLine)" } |
            Set-Content (Join-Path $runDir 'processes.txt')
        # Capturing a hung window blocks until it answers, which a frozen game never does (25 Oct: the
        # launcher waited on it with the game frozen and the mod files still installed).
        if (-not $NoUiAutomation -and $p.Responding) {
            try { & (Join-Path $PSScriptRoot 'screenshot.ps1') -Out (Join-Path $runDir 'window.png') -ProcessId $p.Id } catch { "screenshot failed: $_" }
        } elseif (-not $NoUiAutomation) { 'game not responding: no final screenshot' }
        if (-not $NoClose) {
            if ($NoUiAutomation) { 'test deadline reached; stopping owned game' }
            $closed = Stop-Sm2OwnedGame -Process $ownedGame -NoUiAutomation:$NoUiAutomation
            if (-not $closed) { Write-Warning 'The game is still running; the mod DLL cannot be removed yet.' }
        }
    } else { $closed = $true }
}
catch {
    $primaryFailure=$_
    # Publish before cleanup: a later locked log or cleanup refusal must not
    # erase the launch failure evidence, including failures before adoption.
    try { @{message=$primaryFailure.Exception.Message;exception_type=$primaryFailure.Exception.GetType().FullName;error_id=$primaryFailure.FullyQualifiedErrorId;script_stack=$primaryFailure.ScriptStackTrace;owned_game_adopted=($null -ne $ownedGame)} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'launch-failure.json') }
    catch { Write-Warning "Launch failure receipt could not be saved: $_" }
    Write-Warning "Launch failed before cleanup: $($primaryFailure.Exception.Message)"
    throw $primaryFailure
}
finally {
    if (-not (Get-Sm2OwnedGame $ownedGame)) { $closed = $true }
    if ($captureHelper) {
        if ($closed) {
            try { if (-not (Stop-Sm2CaptureHelper $captureHelper)) { $closed=$false;Write-Warning 'The retained capture helper did not exit; temporary files are retained.' } }
            catch { $closed=$false;Write-Warning "Capture helper cleanup refused: $_" }
        }
        try { @{pid=$captureHelper.Id;created_utc=$(if($captureHelper.CreatedUtc){$captureHelper.CreatedUtc.ToString('o')}else{$null});executable=$captureHelper.Executable;capture_prefix=$captureHelper.CapturePrefix;stdout=$captureHelper.Stdout;stderr=$captureHelper.Stderr;exit_code=$captureHelper.ExitCode;game_pid=$(if($ownedGame){$ownedGame.Id}else{$null})} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'renderdoc-process.json') }
        catch { Write-Warning "Capture process receipt could not be saved: $_" }
    }
    if ($null -ne $ownedGame) {
        try { $gameExit=Get-Sm2OwnedGameExitReceipt $ownedGame;if($gameExit){$gameExit | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'owned-game-exit.json')} }
        catch { Write-Warning "Owned game exit receipt could not be saved: $_" }
    }
    if ($closed -and $null -ne $ownedGame) { $ownedGame.Dispose(); $ownedGame=$null }
    # The owned handle can report the exit while a fresh enumeration still lists the closing
    # process for a few seconds (24 Oct: both evening runs kept their files). Wait for it to go.
    if ($closed) { for ($i = 0; $i -lt 30 -and (Get-ExistingGames); $i++) { Start-Sleep -Seconds 1 } }
    # Another live/unqueryable launch is never adopted, killed or unloaded.
    if ($closed -and (Get-ExistingGames)) { $closed=$false; Write-Warning 'A game process remains live or its exit cannot be confirmed; temporary files are retained.' }
    if ($closed) {
        # Helper processes outlive the game window for a few seconds and keep the DLL locked.
        for ($i = 0; $i -lt 30 -and (Get-BinProcesses); $i++) { Start-Sleep -Seconds 1 }
        # Do not kill unrelated bin-folder processes to unlock a file. Failed removal remains explicit.
        for ($i = 0; $i -lt 10 -and (Test-Path $target); $i++) {
            Remove-Item $target -Force -ErrorAction SilentlyContinue
            if (Test-Path $target) { Start-Sleep -Seconds 1 }
        }
        foreach ($f in $droppings) {
            $src = Join-Path $gameBin $f
            if (Test-Path $src) { Move-Item $src (Join-Path $runDir $f) -Force }
        }
        if (Test-Path $target) { Write-Warning "MOD DLL STILL INSTALLED: $target. Run uninstall-mod.ps1 before starting the game from Steam." }
        else { 'removed mod dll: True' }
        if ($dataPakReceipt) { "removed test data pack: $(Remove-Sm2TestDataPak $dataPakReceipt)" }
        if ($runtimeApiReceipt) { "runtime api cleanup: $(Remove-Sm2RuntimeApi $runtimeApiReceipt)" }
        if ($videoReceipt) {
            try { $videoRestored = Restore-Sm2VideoSettings $videoReceipt } catch { $videoRestored = $false; Write-Warning "Video settings restore failed: $_" }
            "restored video settings: $videoRestored"
            if (-not $videoRestored) { Write-Warning "VIDEO SETTINGS STILL OVERRIDDEN. Run scripts\restore-video-settings.ps1; launches refuse until then." }
        }
    } else {
        "game left running; $target is still installed"
        if ($dataPakReceipt) { "test data pack still installed: $($dataPakReceipt.Path)" }
        if ($videoReceipt) { Write-Warning "Video settings stay overridden while the game runs. Close it, then run scripts\restore-video-settings.ps1." }
    }
    Get-ChildItem $logDir -Filter 'sm2vr*.log' -ErrorAction SilentlyContinue | ForEach-Object { try { Move-Item -LiteralPath $_.FullName -Destination $runDir -Force } catch { Write-Warning "Log archive could not be saved: $_" } }
    # Playtest trace (SM2VR_TRACE=1); summarise with scripts\analyze_vr_trace.py.
    Get-ChildItem $logDir -Filter 'sm2vr_trace*.jsonl' -ErrorAction SilentlyContinue | ForEach-Object { try { Move-Item -LiteralPath $_.FullName -Destination $runDir -Force } catch { Write-Warning "Trace archive could not be saved: $_" } }
    if ($videoReceipt) {
        # Did the game actually take the override? The last logged swap chain is its output size.
        try {
            $observed = Get-Sm2ObservedSwapChain (Join-Path $runDir 'sm2vr.log')
            $expectedWidth = if ($videoOverride.OutputWidth) { $videoOverride.OutputWidth } else { $videoOverride.Width }
            $expectedHeight = if ($videoOverride.OutputHeight) { $videoOverride.OutputHeight } else { $videoOverride.Height }
            $matched = if ($null -eq $observed -or -not $expectedWidth) { $null } else { $observed.Width -eq $expectedWidth -and $observed.Height -eq $expectedHeight }
            "video override observed swap chain: $(if ($observed) { "$($observed.Width)x$($observed.Height) ($($observed.Reason))" } else { 'not logged' })$(if ($null -ne $matched) { " requested $($expectedWidth)x$($expectedHeight) match=$matched" })"
            @{requested=$videoOverride;observed_swap_chain=$observed;resolution_matched=$matched;restored=$videoRestored} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDir 'video-settings-result.json')
        } catch { Write-Warning "Video override result could not be saved: $_" }
        # A matching swap chain is not enough: Joe's 21 Oct run had one and still showed a cropped,
        # zoomed eye. Check the eye copies, native colour targets and upscaler input too.
        $python = Join-Path $root '.venv\Scripts\python.exe'
        if ($videoOverride.OutputWidth -and -not (Test-Path -LiteralPath $python)) {
            # A git worktree has no .venv of its own; use the main checkout's (as for the grenade layout).
            try { $common = git -C $root rev-parse --path-format=absolute --git-common-dir; if ($common) { $python = Join-Path (Split-Path $common -Parent) '.venv\Scripts\python.exe' } } catch {}
        }
        $dump = $env:SM2VR_ORDINARY_DUMP_DIR
        if ($videoOverride.OutputWidth -and $dump -and (Test-Path -LiteralPath $python) -and (Test-Path -LiteralPath (Join-Path $runDir 'sm2vr.log'))) {
            try {
                $check = & $python -I (Join-Path $PSScriptRoot 'check_output_size_run.py') (Join-Path $runDir 'sm2vr.log') $dump "$($videoOverride.OutputWidth)x$($videoOverride.OutputHeight)" 2>&1
                $check | Set-Content -LiteralPath (Join-Path $runDir 'output-size-check.txt')
                $check | ForEach-Object { "output size check: $_" }
            } catch { Write-Warning "Output size check could not run: $_" }
        }
    }
    "run folder: $runDir"
}
