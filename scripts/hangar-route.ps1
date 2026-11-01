# Runs a scripted route on a game already standing in the hangar with -TestDriver -ScriptedHead
# (drive-game.ps1 -Launch ... -Do 'continue,keys:ESC,wait:2,keys:ESC' gets it there), recording both
# eyes from the SteamVR mirror for each step. The flicker and own-queue runs use it
# (docs/VIEWER_FLICKER_2026-10-27.md, docs/OPENXR_OWN_QUEUE.md).
#
# Usage: hangar-route.ps1 -OutDir <abs dir> [-Route 'still,fp,fp_back,enter,drag,fast,exit'] [-Name a1]
#   still    config/test_driver/hangar_still.json (40 s, recorded)
#   fp       viewer_flicker_fp_walk.json, fp_back viewer_flicker_fp_walk_back.json (first person, recorded)
#   enter    animcam_viewer_enter_raised.json (camera mode on; not recorded)
#   drag, far, fast  viewer_flicker_{drag,far,fast}_raised.json (camera mode, recorded)
#   exit     animcam_viewer_exit_raised.json (camera mode off; not recorded)
#   rotate   hangar_rotate.json (stand still, head yaw sweeps at three speeds, 31 s, recorded)
#   spin     hangar_spin.json (full head turns one way at a time, 60/120/180 deg/s, 24 s, recorded)
#   camhold  viewer_hold_raised.json (camera mode: the end pose of fast held 15 s, recorded; run it after fast)
#   spinlook hangar_spin_look.json (full head turns looking down at the deck and up, 22 s, recorded)
#   rotlow   hangar_rotate_low.json (slow 20 deg/s head turns, hands lowered out of view, 35 s, recorded)
#   strafe   hangar_strafe.json (head strafes 1.8 m left/right, view fixed, 24 s, recorded; first person or camera mode)
#   hold     hangar_hold.json (head still 15 s, recorded; after enter it is a still camera-mode view)
#   <step>@<n>  record that step at 1/n size (1, 2, 4, 8 or 16) instead of -Downscale, e.g. rotate@8
#   eye:<native|swap|left>   write eye_source.txt (launch with -EyeSourceControl); not a step of its own
#   shading:<mode>           write shading.txt; a full mode name or an alias from $shadingAlias below
#   async:<native|graphics|no_async>  write async_lighting.txt (launch with -AsyncLightingControl; labels agfx, anoasync)
#   lights:<native|copy>  write copy_lights.txt (launch with -LightListCopyControl; label lc)
#   capsort:<native|distance>  write cap_sort.txt (launch with -LightCapSortControl; label csd)
#   stereo:<capsort|shared|copy|native>  write stereo_lights.txt (every launch has it; labels sl-shared, sl-copy, sl-native;
#            capsort, the default, has no label)
#   flare:<native|hide>  write flare_hide.txt (launch with -FlareHideControl; label fh): lamp flares drawn at zero visibility
#   dump:<n>  write light_dump.txt: the next n light jobs' raw view lists go to light_dump_*.bin in the dump folder
#            (scripts/analyze_light_dump.py; only while stereo lights are 'shared')
#   clamp:<native|clamp>  write light_clamp.txt (launch with -LightClampControl; label c4)
#   plain:<native|plain>  write plain_test.txt (launch with -SecondCameraPlainTestControl; label p1)
#   farlight:<native|off>  write far_light.txt (launch with -FarLightCacheControl; label fcoff)
#   harvest:<native|once>  write harvest.txt (launch with -HarvestOnceControl; label h1)
#   section:<slot|native>  write light_section.txt (every launch; slot, the default, has no label; native = ls-native)
#   lighthold:<frames>[d]  write light_hold.txt (launch with -LightHoldControl; label lh<frames>[d], 0 = off;
#            d = "drops": also hold results under 4, not only missing ones)
#   Recorded steps after an eye or shading token carry it in their name when it is not the default:
#   4rotate-left-lv is the fourth step, rotate, with eye source left and shading light_visibility.
#   The tokens need -ControlDir, the launch's dump folder (build\ordinary\<date>\<tag>). The mod polls
#   the files about once a second, so the script waits 3 s after writing one.
# Each recorded step writes <OutDir>\<Name>_<n><step>.rgb/.json/.times (openvr_mirror_capture stream, 1/16
# size) and a row in <OutDir>\segments.csv: name, step, start and end wall-clock time, final driver state.
# Analyse: scripts\viewer_flicker.py <prefix>.rgb <run dir>; pair rate per row with
# scripts\summarize_session_rate.py <run dir>\sm2vr.log --start <start> --end <end>.
param([Parameter(Mandatory)][string]$OutDir, [string]$Route = 'still,fp,fp_back,enter,drag,fast,exit',
    [string]$Name = 'run', [int]$Downscale = 16, [string]$ControlDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $root '.venv\Scripts\python.exe'
$driver = Join-Path $PSScriptRoot 'vr_test_driver.py'
$mirror = Join-Path $root 'build\mod\openvr_mirror_capture.exe'
$scenarios = @{
    still = 'hangar_still'; fp = 'viewer_flicker_fp_walk'; fp_back = 'viewer_flicker_fp_walk_back'
    enter = 'animcam_viewer_enter_raised'; exit = 'animcam_viewer_exit_raised'
    drag = 'viewer_flicker_drag_raised'; far = 'viewer_flicker_far_raised'; fast = 'viewer_flicker_fast_raised'
    rotate = 'hangar_rotate'; hold = 'hangar_hold'; spin = 'hangar_spin'; camhold = 'viewer_hold_raised'; spinlook = 'hangar_spin_look'; rotlow = 'hangar_rotate_low'; strafe = 'hangar_strafe'
}
$shadingAlias = @{
    lusr = 'light_union_shadow_refresh'; lu = 'light_union'; lv = 'light_visibility'; none = 'none'
    nsl = 'light_union_shadow_refresh_native_shadow_lights'; sss = 'light_union_shadow_refresh_shared_spot_shadows'
    nvol = 'light_union_shadow_refresh_native_volumetrics'; all = 'light_union_shadow_refresh_all'
    ssr = 'light_union_shadow_refresh_ssr'; ssao = 'light_union_shadow_refresh_ssao'; ssh = 'light_union_shadow_refresh_screen_shadows'
}
$eyeLabel = ''; $shadingLabel = ''; $asyncLabel = ''; $holdLabel = ''; $harvestLabel = ''; $sectionLabel = ''; $farLabel = ''; $plainLabel = ''; $clampLabel = ''; $capLabel = ''; $copyLabel = ''; $stereoLabel = ''; $flareLabel = ''
$unrecorded = @('enter', 'exit')
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$csv = Join-Path $OutDir 'segments.csv'
if (-not (Test-Path $csv)) { 'name,step,start,end,state' | Out-File -Encoding ascii $csv }

function Driver-Status {
    # status.json can be read mid-rewrite; the driver's traceback on stderr is a terminating error in
    # PowerShell 5.1 under 'Stop', so a bad read just counts as no status this poll.
    $ErrorActionPreference = 'Continue'
    try {
        $text = & $python -I $driver status 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $text) { return $null }
        return ($text | Out-String | ConvertFrom-Json)
    } catch { return $null }
}

$n = 0
foreach ($token in $Route.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }) {
    if ($token -match '^lights:(native|copy)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'copy_lights.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -LightListCopyControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $copyLabel = if ($Matches[1] -eq 'copy') { 'lc' } else { '' }
        Write-Output "$Name lights: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^flare:(native|hide)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'flare_hide.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -FlareHideControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $flareLabel = if ($Matches[1] -eq 'hide') { 'fh' } else { '' }
        Write-Output "$Name flares: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^dump:(\d+)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        Set-Content -LiteralPath (Join-Path $ControlDir 'light_dump.txt') -Value $Matches[1] -Encoding ascii
        Write-Output "$Name light dump: $($Matches[1]) jobs"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^stereo:(shared|copy|capsort|native)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'stereo_lights.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: the launcher writes it on every stereo launch" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $stereoLabel = if ($Matches[1] -eq 'capsort') { '' } else { 'sl-' + $Matches[1] }
        Write-Output "$Name stereo lights: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^capsort:(native|distance)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'cap_sort.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -LightCapSortControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $capLabel = if ($Matches[1] -eq 'distance') { 'csd' } else { '' }
        Write-Output "$Name capsort: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^clamp:(native|clamp)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'light_clamp.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -LightClampControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $clampLabel = if ($Matches[1] -eq 'clamp') { 'c4' } else { '' }
        Write-Output "$Name clamp: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^plain:(native|plain)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'plain_test.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -SecondCameraPlainTestControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $plainLabel = if ($Matches[1] -eq 'plain') { 'p1' } else { '' }
        Write-Output "$Name plain: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^farlight:(native|off)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'far_light.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -FarLightCacheControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $farLabel = if ($Matches[1] -eq 'off') { 'fcoff' } else { '' }
        Write-Output "$Name farlight: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^section:(slot|native)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        Set-Content -LiteralPath (Join-Path $ControlDir 'light_section.txt') -Value $Matches[1] -Encoding ascii
        $sectionLabel = if ($Matches[1] -eq 'native') { 'ls-native' } else { '' }
        Write-Output "$Name light section: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^harvest:(native|once)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'harvest.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -HarvestOnceControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $harvestLabel = if ($Matches[1] -eq 'once') { 'h1' } else { '' }
        Write-Output "$Name harvest: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^lighthold:(\d+)(d?)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'light_hold.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -LightHoldControl" }
        Set-Content -LiteralPath $control -Value ($Matches[1] + $(if ($Matches[2]) { ' drops' } else { '' })) -Encoding ascii
        $holdLabel = if ($Matches[1] -eq '0') { '' } else { "lh$($Matches[1])$($Matches[2])" }
        Write-Output "$Name lighthold: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^async:(native|graphics|no_async)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $control = Join-Path $ControlDir 'async_lighting.txt'
        if (-not (Test-Path $control)) { throw "$control is missing: launch with -AsyncLightingControl" }
        Set-Content -LiteralPath $control -Value $Matches[1] -Encoding ascii
        $asyncLabel = @{ native = ''; graphics = 'agfx'; no_async = 'anoasync' }[$Matches[1]]
        Write-Output "$Name async: $($Matches[1])"
        Start-Sleep -Seconds 3
        continue
    }
    if ($token -match '^(eye|shading):(\w+)$') {
        if (-not $ControlDir) { throw "$token needs -ControlDir" }
        $kind = $Matches[1]; $value = $Matches[2]
        if ($kind -eq 'eye') {
            if ($value -notin @('native', 'swap', 'left')) { throw "unknown eye source '$value'" }
            $control = Join-Path $ControlDir 'eye_source.txt'
            if (-not (Test-Path $control)) { throw "$control is missing: launch with -EyeSourceControl" }
            Set-Content -LiteralPath $control -Value $value -Encoding ascii
            $eyeLabel = if ($value -eq 'native') { '' } else { $value }
        } else {
            $mode = if ($shadingAlias.ContainsKey($value)) { $shadingAlias[$value] } else { $value }
            Set-Content -LiteralPath (Join-Path $ControlDir 'shading.txt') -Value $mode -Encoding ascii
            $alias = @($shadingAlias.Keys | Where-Object { $shadingAlias[$_] -eq $mode })[0]
            $shadingLabel = if ($mode -eq 'light_union_shadow_refresh') { '' } elseif ($alias) { $alias } else { $mode }
        }
        Write-Output "$Name ${kind}: $value"
        Start-Sleep -Seconds 3
        continue
    }
    $step, $scale = $token.Split('@', 2)
    $scale = if ($scale) { [int]$scale } else { $Downscale }
    if (-not $scenarios.ContainsKey($step)) { throw "unknown step '$step'" }
    if ($scale -notin @(1, 2, 4, 8, 16)) { throw "downscale must be 1, 2, 4, 8 or 16: $token" }
    $label = (@($step, $eyeLabel, $shadingLabel, $asyncLabel, $holdLabel, $harvestLabel, $sectionLabel, $farLabel, $plainLabel, $clampLabel, $capLabel, $copyLabel, $stereoLabel, $flareLabel) | Where-Object { $_ }) -join '-'
    $n++
    $file = Join-Path $root ("config\test_driver\" + $scenarios[$step] + '.json')
    $duration = ((Get-Content -Raw $file | ConvertFrom-Json).duration_ms) / 1000.0
    $capture = $null
    if ($unrecorded -notcontains $step) {
        $prefix = Join-Path $OutDir ('{0}_{1}{2}' -f $Name, $n, $label)
        $capture = Start-Process -FilePath $mirror -ArgumentList @("`"$prefix`"", 'stream', [string][math]::Ceiling($duration + 2), [string]$scale) `
            -RedirectStandardOutput "$prefix.out.txt" -RedirectStandardError "$prefix.err.txt" -WindowStyle Hidden -PassThru
        Start-Sleep -Milliseconds 500
    }
    $start = Get-Date
    $queued = & $python -I $driver run $file | Out-String | ConvertFrom-Json
    $state = 'unknown'
    $deadline = $start.AddSeconds($duration + 30)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $s = Driver-Status
        if ($s -and $s.id -eq $queued.id -and $s.state -notin @('preroll', 'running')) { $state = $s.state; break }
    }
    $end = Get-Date
    if ($capture) { $capture.WaitForExit(15000) | Out-Null; if (-not $capture.HasExited) { $capture.Kill() } }
    '{0},{1},{2},{3},{4}' -f $Name, "$n$label", $start.ToString('HH:mm:ss'), $end.ToString('HH:mm:ss'), $state | Out-File -Append -Encoding ascii $csv
    Write-Output ('{0} {1}{2}: {3} ({4:N0} s)' -f $Name, $n, $label, $state, ($end - $start).TotalSeconds)
    Start-Sleep -Seconds 2
}
