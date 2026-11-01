# Opt-in single-render-job stereo prototype. The launcher restores its temporary
# game files in finally. UI navigation belongs to Computer Use or the user.
# -Controls adds the 17 October Index controls set (controllers, action manifest, hands,
# body following, wrist weapon aim, holsters, game rumble on the controllers unless -NoHaptics)
# and the vr_controls data pack.
# -Gestures adds the left-hand guard and grenade (needs -Controls), with grenades launched along
# the VR gaze/hand (-NoGrenadeAim leaves the game's camera aim); -Trace writes the
# playtest trace (docs/VR_TRACE.md). Calibration/config files live in config\.
# Rule (docs/FLAGS.md): a feature the installer ships is on by default here, with a -NoX opt-out.
# The game HUD (SM2VR_NATIVE_HUD) and the upright torso are on by default since 27 October, as in the
# installer's profile; -NoCompactHud / -NoUprightTorso opt out (playtests launched without the old
# opt-in switches had no HUD, 25-27 October). The old no-op opt-ins were removed on 27 October.
# -Resolution/-WindowMode/-RenderResolution/-Antialiasing override the saved video settings for this launch only
# (docs/RESOLUTION.md); -GameArgs appends engine option tokens after the -DirectLaunch flags.
# -PrintEnv prints the SM2VR_* environment as JSON instead of launching (installer/app/sm2-profile.ps1 parity).
# -PerfCampaign is the campaign profiling recipe (docs/validation/campaign-perf-plan-2026-10-21.md):
# -Profile -PerformanceProbes, a flat/stereo A/B control (scripts\stereo-hold.ps1) and a hidden
# companion that records PresentMon, nvidia-smi and 20 Hz thread stacks next to cpu_trace.csv.
param([int]$Seconds=900,[string]$Tag='ordinary_headset',[switch]$DirectLaunch,[switch]$CapturePixels,[switch]$ShareVolumeFrame,[switch]$Profile,[switch]$ShadingControl,
    [switch]$Controls,[switch]$Gestures,[double]$HapticsGain=1,[switch]$NoHaptics,[switch]$Trace,[switch]$NoUprightTwist,[switch]$KeepHeadVisible,[switch]$PhotoNativePose,[switch]$BodyPlacementCandidate,[switch]$PlayerPartVisibilityCandidate,[switch]$EyeSourceControl,[switch]$AsyncLightingControl,[switch]$LightHoldControl,[switch]$HarvestOnceControl,[switch]$FarLightCacheControl,[switch]$SecondCameraPlainTestControl,[switch]$LightListLog,[switch]$LightClampControl,[switch]$LightCapSortControl,[switch]$LightListCopyControl,[switch]$ToneSourceControl,[switch]$FlareProbe,[switch]$FlareHideControl,[switch]$NoAdaptationMirror,[switch]$AdaptationControl,[int]$SecondCameraId=-1,[switch]$NativeHzb,[switch]$NoTrackerGuard,[int]$StereoWarmupJobs=0,[switch]$InvalidateArmPoseCandidate,[switch]$ResolveArmPoseCandidate,[switch]$TestDriver,[switch]$ScriptedHead,[switch]$EyeSnapshots,[switch]$PlayerDistanceFade,[switch]$TakeoverControl,[switch]$NoChainswordRev,[switch]$NativeShadows,[switch]$NoGrenadeAim,[switch]$GrenadeVelocityProbe,[switch]$NoGazeCamera,[switch]$GuardGuide,
    [ValidateSet('OpenVR','OpenXR')][string]$Runtime='OpenXR',[ValidateSet('shared','copy','capsort','native')][string]$StereoLights='capsort',[ValidateSet('play','photo','off')][string]$ThumbsUp='play',[ValidateSet('replay','animation','hand')][string]$MeleeSweep='replay',[switch]$GripFitLog,[switch]$CopyAllocationControl,[switch]$PerformanceProbes,[switch]$HandoffControl,[switch]$QueryDependencies,[switch]$DrawVisibilityProbe,[switch]$WorkerVisibilityProbe,[switch]$RenderDocCapture,[int]$InjectStereoSuspendSeconds=0,[int]$InjectRecordsMoveSeconds=0,
    [string]$Resolution='',[string]$WindowMode='',[string]$RenderResolution='',[string]$Antialiasing='',[string]$GameArgs='',[string]$DesktopWindow='',[string]$OutputSize='',[string]$OutputSizeMode='',[switch]$ExperimentalOutputSize,[switch]$PerfCampaign,[ValidateRange(0,50)][int]$PerfSampleHz=20,[switch]$QueueTimeline,[switch]$OpenXrGameQueue,[switch]$NoOpenXrSubmitThread,[switch]$ChaseCam,[switch]$PrintEnv,[switch]$AutoContinue,[string]$InGameKeys='',[string]$EnvOff='',[string]$EnvSet='',[string]$EffectsPack='',[ValidateSet('','off','native','left_only','shared_objects','shared')][string]$HzbControl='',[switch]$NoCompactHud,[switch]$NoUprightTorso)
# Without [CmdletBinding()] PowerShell drops unknown switches into $args; a misspelt or removed
# switch must stop the launch instead of silently running something else (docs/FLAGS.md).
if ($args.Count) { throw "Unknown launcher arguments: $($args -join ' ') (switches: docs/FLAGS.md)" }
# Every switch's class, as in docs/FLAGS.md (tests/test_launch_flags.py keeps the two equal). The
# shipped switches pick the player's view; any other switch away from its default is listed in the
# run's launch-flags.json and its "launch flags:" line, so a log shows whether it ran what players get.
$switchClass = [ordered]@{
    'shipped' = @('Controls','Gestures','ChaseCam')
    'opt-out' = @('NoChainswordRev','NoCompactHud','NoGazeCamera','NoGrenadeAim','NoUprightTorso','NoHaptics','HapticsGain','KeepHeadVisible','PhotoNativePose','ThumbsUp','MeleeSweep','GuardGuide','NativeShadows','Trace','Runtime')
    'diagnostic' = @('HzbControl','StereoLights','CapturePixels','ShareVolumeFrame','ShadingControl','NoUprightTwist','BodyPlacementCandidate','PlayerPartVisibilityCandidate','EyeSourceControl','AsyncLightingControl','LightHoldControl','HarvestOnceControl','FarLightCacheControl','SecondCameraPlainTestControl','LightListLog','LightClampControl','LightCapSortControl','LightListCopyControl','ToneSourceControl','FlareProbe','NoAdaptationMirror','AdaptationControl','SecondCameraId','NativeHzb','NoTrackerGuard','StereoWarmupJobs','InvalidateArmPoseCandidate','ResolveArmPoseCandidate','PlayerDistanceFade','TakeoverControl','GrenadeVelocityProbe','CopyAllocationControl','HandoffControl','DrawVisibilityProbe','WorkerVisibilityProbe','InjectStereoSuspendSeconds','InjectRecordsMoveSeconds','OutputSize','OutputSizeMode','ExperimentalOutputSize','OpenXrGameQueue','NoOpenXrSubmitThread','EnvOff','EnvSet')
    'dev tool' = @('Seconds','Tag','PrintEnv','DirectLaunch','Profile','PerformanceProbes','QueryDependencies','QueueTimeline','PerfCampaign','PerfSampleHz','FlareHideControl','EffectsPack','TestDriver','ScriptedHead','EyeSnapshots','RenderDocCapture','GripFitLog','Resolution','WindowMode','RenderResolution','Antialiasing','DesktopWindow','GameArgs','AutoContinue','InGameKeys')
}
# Run bookkeeping, not configuration; -EnvSet/-EnvOff are listed by variable instead.
$unlistedSwitches = @('Seconds','Tag','PrintEnv','EnvSet','EnvOff')
$launcherParameters = $MyInvocation.MyCommand.ScriptBlock.Ast.ParamBlock.Parameters
if ($PerfCampaign) { $Profile=$true; $PerformanceProbes=$true }
if ($PerfSampleHz -ne 20 -and -not $PerfCampaign) { throw '-PerfSampleHz needs -PerfCampaign.' }
if ($WorkerVisibilityProbe -and -not $DrawVisibilityProbe) { throw '-WorkerVisibilityProbe needs -DrawVisibilityProbe (bounded producer-mask diagnostics).' }
if ($DrawVisibilityProbe -and (-not $EyeSnapshots -or -not $TestDriver)) { throw '-DrawVisibilityProbe needs -EyeSnapshots and -TestDriver (bounded CPU/image diagnostics).' }
if ($QueryDependencies -and -not $PerformanceProbes) { throw '-QueryDependencies needs -PerformanceProbes.' }
if ($QueueTimeline -and -not $Profile) { throw '-QueueTimeline needs -Profile (or -PerfCampaign).' }
if ($OpenXrGameQueue -and $Runtime -ne 'OpenXR') { throw '-OpenXrGameQueue needs -Runtime OpenXR.' }
if ($NoOpenXrSubmitThread -and $Runtime -ne 'OpenXR') { throw '-NoOpenXrSubmitThread needs -Runtime OpenXR.' }
if ($PerformanceProbes -and -not $Profile) { throw '-PerformanceProbes needs -Profile.' }
if ($ChaseCam -and $Controls) { throw '-ChaseCam is gamepad-only and cannot be combined with -Controls (the stock chase camera has no head camera for hands or gestures).' }
if ($TestDriver -and -not $Controls) { throw '-TestDriver needs -Controls.' }
if ($TakeoverControl -and -not $Controls) { throw '-TakeoverControl needs -Controls (diagnostic animation takeover on body following).' }
if ($GripFitLog -and -not ($Controls -and $Runtime -eq 'OpenXR')) { throw '-GripFitLog needs -Runtime OpenXR -Controls (phase 0 grip-offset fit input).' }
if ($ScriptedHead -and -not $TestDriver) { throw '-ScriptedHead needs -TestDriver (unattended head motion only).' }
if ($Gestures -and -not $Controls) { throw '-Gestures needs -Controls (controllers, hands and body following).' }
if ($NoGrenadeAim -and -not $Gestures) { throw '-NoGrenadeAim needs -Gestures (grenade aim is part of the grenade gesture).' }
if ($GrenadeVelocityProbe -and ($NoGrenadeAim -or -not $Gestures)) { throw '-GrenadeVelocityProbe needs -Gestures with grenade aim on (native launch velocity samples).' }
if ($NoChainswordRev -and -not $Controls) { throw '-NoChainswordRev needs -Controls (the analog chainsword rev only exists with controls).' }
if ($NoGazeCamera -and -not $Controls) { throw '-NoGazeCamera needs -Controls (the gaze camera only exists with controls).' }
if ($GuardGuide -and -not $Controls) { throw '-GuardGuide needs -Controls (the guard guide only exists with controls).' }
if ($NoHaptics -and -not $Controls) { throw '-NoHaptics needs -Controls (haptics only exist with controls).' }
if ($HapticsGain -lt 0 -or $HapticsGain -gt 2) { throw '-HapticsGain must be 0..2.' }
if ($NoCompactHud -and -not ($Controls -or $ChaseCam)) { throw '-NoCompactHud needs -Controls or -ChaseCam (the game HUD only exists there).' }
if ($NoUprightTorso -and -not $Controls) { throw '-NoUprightTorso needs -Controls (the upright torso only exists with controls).' }
$nativeHudOn = ($Controls -or $ChaseCam) -and -not $NoCompactHud
$uprightTorsoOn = $Controls -and -not $NoUprightTorso
if ($NoUprightTwist -and -not $uprightTorsoOn) { throw '-NoUprightTwist needs the upright torso (-Controls without -NoUprightTorso); it keeps the lean correction and leaves the native shoulder twist (A/B for docs/PAULDRON.md).' }
if ($KeepHeadVisible -and -not $Controls) { throw '-KeepHeadVisible needs -Controls (diagnostic comparison of existing head hiding).' }
if ($PhotoNativePose -and -not $Controls) { throw '-PhotoNativePose needs -Controls (photo mode keeps the game''s own arm pose instead of the player''s).' }
if ($ThumbsUp -ne 'play' -and -not $Controls) { throw '-ThumbsUp needs -Controls (it shapes the finger-tracked hands).' }
if ($MeleeSweep -ne 'replay' -and -not $Controls) { throw '-MeleeSweep needs -Controls (it picks the sword the melee hit check sweeps).' }
if ($PlayerPartVisibilityCandidate -and -not $Controls) { throw '-PlayerPartVisibilityCandidate needs -Controls (reversible named-part shadow-only diagnostic).' }
if ($InvalidateArmPoseCandidate -and -not $Controls) { throw '-InvalidateArmPoseCandidate needs -Controls (experimental post-write native pose invalidation).' }
if ($ResolveArmPoseCandidate -and -not $Controls) { throw '-ResolveArmPoseCandidate needs -Controls (experimental post-write native world evaluation).' }
if ($ResolveArmPoseCandidate -and $InvalidateArmPoseCandidate) { throw 'Select either -ResolveArmPoseCandidate or -InvalidateArmPoseCandidate.' }
if ($BodyPlacementCandidate -and -not $Controls) { throw '-BodyPlacementCandidate needs -Controls (experimental whole-body render placement before arm IK).' }
if ($InjectStereoSuspendSeconds -lt 0 -or ($InjectStereoSuspendSeconds -gt 0 -and $InjectStereoSuspendSeconds -lt 5)) { throw '-InjectStereoSuspendSeconds is 0 (off) or at least 5 (diagnostic: one forced stereo suspension per period).' }
if ($InjectRecordsMoveSeconds -lt 0 -or ($InjectRecordsMoveSeconds -gt 0 -and $InjectRecordsMoveSeconds -lt 5)) { throw '-InjectRecordsMoveSeconds is 0 (off) or at least 5 (diagnostic: one simulated native record-table move per period).' }
# The eyes render at the game's output size (-Resolution); -DesktopWindow only shrinks the desktop
# window, which the game stretches its output into (docs/EYE_SIZE.md). Same strict form as the mod.
# -OutputSize: the mod sets the game's render output (and so each eye) itself, from the proxy's
# start, whatever the saved settings say (docs/EYE_SIZE.md). run-modded.ps1 validates the size.
if ($OutputSizeMode -and -not $OutputSize) { throw '-OutputSizeMode needs -OutputSize.' }
# Diagnostic only: on 21 Oct the eyes were the left half of a 4K frame (2x horizontal zoom, unplayable).
if ($OutputSize -and -not $ExperimentalOutputSize) { throw '-OutputSize is a diagnostic that currently crops and zooms the eyes (docs/EYE_SIZE.md); add -ExperimentalOutputSize to run it, and never for play.' }
if ($ExperimentalOutputSize -and -not $OutputSize) { throw '-ExperimentalOutputSize needs -OutputSize.' }
if ($OutputSizeMode -and $OutputSizeMode -cnotin @('device','config')) { throw "-OutputSizeMode must be device or config (got '$OutputSizeMode')." }
if ($DesktopWindow) {
    if ($DesktopWindow -cnotmatch '^[1-9][0-9]{2,3}x[1-9][0-9]{2,3}$') { throw "-DesktopWindow must look like 960x540 (got '$DesktopWindow')." }
    if ($WindowMode -ne 'Windowed') { throw '-DesktopWindow needs -WindowMode Windowed (fullscreen and borderless windows are never resized).' }
}
if ($NativeShadows -and $ShadingControl) { throw '-NativeShadows is implied by -ShadingControl (native-effect isolation starts with every fix off).' }
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$saved=@{}
Get-ChildItem Env:SM2VR_* | ForEach-Object { $saved[$_.Name]=$_.Value }
try {
    Get-ChildItem Env:SM2VR_* | Remove-Item
    foreach($name in @('OBSERVER','SECOND_CAMERA','PAIR_COLOUR','PAIR_RETENTION','PRIVATE_DLSS','NATIVE_PACKET','TEMPORAL_PROBE','NATIVE_CREATE','NATIVE_BLOOM','TAA','CONSECUTIVE','FAULT_POD','SHARED_GRADING','SHARED_SHARPEN_EXPOSURE','SKIP_LATE_EFFECT','DISABLE_EARLY_EFFECT','HEADSET')) {
        Set-Item -LiteralPath ('Env:SM2VR_ORDINARY_'+$name) -Value '1'
    }
    if ($Runtime -eq 'OpenXR') { $env:SM2VR_OPENXR='1' } else { $env:SM2VR_OPENVR='1' }
    # Experimental A/B (docs/validation/depth-ready-fence-2026-10-21.md): bind the OpenXR session and our
    # swapchain copies to a mod-owned queue, so runtime-side GPU waits cannot hold the game's queue.
    if ($OpenXrGameQueue) { $env:SM2VR_OPENXR_GAME_QUEUE='1' }
    if ($NoOpenXrSubmitThread) { $env:SM2VR_OPENXR_NO_SUBMIT_THREAD='1' }
    $env:SM2VR_STEREO='1'
    if ($NativeHzb) { $env:SM2VR_NATIVE_HZB='1' }
    if ($NoTrackerGuard) { $env:SM2VR_TRACKER_GUARD='0' }
    if ($StereoWarmupJobs -gt 0) { $env:SM2VR_STEREO_WARMUP_JOBS=[string]$StereoWarmupJobs }
    # Diagnostic: refuse one valid bloom contract per period to exercise suspend, flat panel and resume live.
    if ($InjectStereoSuspendSeconds -gt 0) { $env:SM2VR_DIAG_STEREO_SUSPEND_S=[string]$InjectStereoSuspendSeconds }
    # Diagnostic: a stale record-table address in the bloom contract, so the live probe must re-base (the 21 Oct table move).
    if ($InjectRecordsMoveSeconds -gt 0) { $env:SM2VR_DIAG_RECORDS_MOVE_S=[string]$InjectRecordsMoveSeconds }
    if ($PlayerDistanceFade) { $env:SM2VR_PLAYER_DISTANCE_FADE='1' }
    if ($CapturePixels) { $env:SM2VR_ORDINARY_SDK_PIXELS='1' }
    if ($ShareVolumeFrame) { $env:SM2VR_ORDINARY_SHARED_VOLUME_FRAME='1' }
    $env:SM2VR_CALIBRATION=Join-Path $repo 'config\index.calibration'
    $env:SM2VR_CAMERA_CONFIG=Join-Path $repo 'config\index_camera_modes.calibration'
    $env:SM2VR_ORDINARY_DUMP_DIR=Join-Path $repo ('build\ordinary\'+(Get-Date -Format 'yyyy-MM-dd')+'\'+$Tag)
    New-Item -ItemType Directory -Force -Path $env:SM2VR_ORDINARY_DUMP_DIR | Out-Null
    if ($CopyAllocationControl) {
        $env:SM2VR_INLINE_COPY_ALLOCATION_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'allocation.txt'
        Set-Content -LiteralPath $env:SM2VR_INLINE_COPY_ALLOCATION_CONTROL -Value 'pooled' -Encoding ascii
    }
    if ($PlayerDistanceFade) {
        $env:SM2VR_PLAYER_DISTANCE_FADE_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'player_fade.txt'
        Set-Content -LiteralPath $env:SM2VR_PLAYER_DISTANCE_FADE_CONTROL -Value 'opaque' -Encoding ascii
    }
    if ($TakeoverControl) {
        # Diagnostic (docs/ANIMATION_CAMERA.md): write 'takeover' to feed body following a pulled-back,
        # bobbing animation camera (the game's own camera is untouched); 'off' restores the real one.
        $env:SM2VR_DIAG_TAKEOVER_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'takeover.txt'
        Set-Content -LiteralPath $env:SM2VR_DIAG_TAKEOVER_CONTROL -Value 'off' -Encoding ascii
    }
    if ($EyeSnapshots) { $env:SM2VR_EYE_DUMP_DIR=$env:SM2VR_ORDINARY_DUMP_DIR }
    if ($DrawVisibilityProbe) { $env:SM2VR_ORDINARY_DRAW_VISIBILITY=$env:SM2VR_ORDINARY_DUMP_DIR }
    if ($WorkerVisibilityProbe) { $env:SM2VR_ORDINARY_WORKER_VISIBILITY='1' }
    if ($HandoffControl) {
        $env:SM2VR_ORDINARY_HANDOFF_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'handoff.txt'
        Set-Content -LiteralPath $env:SM2VR_ORDINARY_HANDOFF_CONTROL -Value 'present' -Encoding ascii
    }
    if ($Profile) {
        $env:SM2VR_PROFILE=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'cpu_trace.csv'
        # Detailed native per-eye wait scopes exceed the AFR trace budget during a full A/B run.
        $env:SM2VR_PROFILE_MAX_ROWS='10000000'
        if ($PerformanceProbes) { $env:SM2VR_PROFILE_PROBES='1' }
        if ($QueryDependencies) { $env:SM2VR_PROFILE_QUERY_DEPENDENCIES='1' }
        # Every queue submission, fence signal/wait and GPU completion (GpuQueueTimeline.h; scripts\analyze_depth_ready_wait.py).
        if ($QueueTimeline) { $env:SM2VR_PROFILE_QUEUE_TIMELINE='1' }
    }
    if ($PerfCampaign) {
        # Flat/stereo A/B: the mod polls this word once a second ('mono' holds native mono).
        $env:SM2VR_STEREO_HOLD_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'stereo_hold.txt'
        Set-Content -LiteralPath $env:SM2VR_STEREO_HOLD_CONTROL -Value 'stereo' -Encoding ascii -NoNewline
        $perfRoot=Join-Path $repo 'build\perf'; New-Item -ItemType Directory -Force -Path $perfRoot | Out-Null
        @{dump=$env:SM2VR_ORDINARY_DUMP_DIR;stereo_hold_control=$env:SM2VR_STEREO_HOLD_CONTROL;started_utc=(Get-Date).ToUniversalTime().ToString('o');tag=$Tag} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $perfRoot 'current-campaign.json')
        $companion=Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',
            "`"$(Join-Path $PSScriptRoot 'perf-campaign-companion.ps1')`"",'-OutputDirectory',"`"$env:SM2VR_ORDINARY_DUMP_DIR`"",'-SampleHz',$PerfSampleHz)
        "perf campaign: companion pid=$($companion.Id); data in $env:SM2VR_ORDINARY_DUMP_DIR; flat/stereo A/B: scripts\stereo-hold.ps1 -Mode mono|stereo"
    }
    $env:SM2VR_ORDINARY_SHADING_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'shading.txt'
    # Stereo shadow fix (docs/eye-shadow-followup.md): both ordinary eyes refit their sun cascades
    # every frame, so the second eye no longer shades with a stale reused fit. Default since
    # 20 October (Joe saw mismatched eye shadows in the one launch without it). -NativeShadows
    # keeps the game's cascade reuse for A/B.
    $shadingMode = if ($ShadingControl) { 'none' } elseif ($NativeShadows) { 'light_union' } else { 'light_union_shadow_refresh' }
    Set-Content -LiteralPath $env:SM2VR_ORDINARY_SHADING_CONTROL -Value $shadingMode -Encoding ascii
    # Occlusion culling A/B (docs/HZB_STEREO_2026-11-01.md): hzb.txt beside shading.txt switches it live.
    if ($HzbControl) {
        $env:SM2VR_HZB_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'hzb.txt'
        Set-Content -LiteralPath $env:SM2VR_HZB_CONTROL -Value $HzbControl -Encoding ascii
    }
    # Stereo lights (docs/DECIDE_ONCE_STEREO.md): capsort (default) keeps each eye's nearest 240 lights, a
    # stable cut; shared and copy are the merged-list diagnostics. stereo_lights.txt beside shading.txt
    # switches it live (the mod finds it there: no variable, so the environment stays the players').
    if ($StereoLights -ne 'capsort') { $env:SM2VR_STEREO_LIGHTS=$StereoLights }
    Set-Content -LiteralPath (Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'stereo_lights.txt') -Value $StereoLights -Encoding ascii
    # Diagnostic: tone_source.txt picks tone inputs the right eye takes from the left eye (OrdinaryToneSource.h).
    if ($ToneSourceControl) {
        $env:SM2VR_DIAG_TONE_SOURCE_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'tone_source.txt'
        Set-Content -LiteralPath $env:SM2VR_DIAG_TONE_SOURCE_CONTROL -Value 'native' -Encoding ascii
    }
    # Diagnostic: log lamp-flare query state per camera (NativeFlareProbe.h).
    if ($FlareProbe) { $env:SM2VR_DIAG_FLARE_PROBE='1' }
    # Benchmark: flare_hide.txt (hide/native) draws every lamp flare at zero visibility (NativeFlareProbe.h).
    if ($FlareHideControl) {
        $env:SM2VR_DIAG_FLARE_HIDE_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'flare_hide.txt'
        Set-Content -LiteralPath $env:SM2VR_DIAG_FLARE_HIDE_CONTROL -Value 'native' -Encoding ascii
    }
    # The right eye's view takes the HDR core's eye adaptation from index 0, the index both eyes adapt on
    # (NativeAdaptationMirror.h: the hangar right-eye glow). On by default since 23 October; -NoAdaptationMirror
    # opts out. -AdaptationControl adds adaptation_mirror.txt (left/native) to switch it live for A/B.
    if (-not $NoAdaptationMirror) { $env:SM2VR_ADAPTATION_MIRROR='1' }
    if ($AdaptationControl) {
        $env:SM2VR_ADAPTATION_MIRROR_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'adaptation_mirror.txt'
        Set-Content -LiteralPath $env:SM2VR_ADAPTATION_MIRROR_CONTROL -Value $(if ($NoAdaptationMirror) { 'native' } else { 'left' }) -Encoding ascii
    }
    $config=Join-Path $repo 'config'
    $pak=Join-Path $repo 'build\vr_menus\SM2VR_FirstPerson_Test.pak'
    if ($ChaseCam) {
        # The game's own cameras (no first-person camera files) rendered in stereo, gamepad controls (docs/THIRD_PERSON.md).
        $env:SM2VR_CHASE_CAM='1'
        $pak=Join-Path $repo 'build\chase_cam\SM2VR_FirstPerson_Test.pak'
        if (-not $PrintEnv -and -not (Test-Path -LiteralPath $pak)) { throw "Chase-cam pack missing: $pak (build it with scripts\build_chase_cam.py)" }
        if ($nativeHudOn) { $env:SM2VR_NATIVE_HUD='1' }
    }
    if ($Controls) {
        $env:SM2VR_CONTROLLERS='1'
        $env:SM2VR_ACTION_MANIFEST=Join-Path $config 'vr_actions.json'
        # OpenXR grip pose -> raw OpenVR controller origin, so every hand calibration carries over.
        if ($Runtime -eq 'OpenXR') { $env:SM2VR_OPENXR_GRIP_CALIBRATION=Join-Path $config 'openxr_index_grip.calibration' }
        if ($GripFitLog) { $env:SM2VR_OPENXR_GRIP_LOG=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'openxr_grips.csv' }
        $env:SM2VR_SNAP_DEGREES='30'
        $env:SM2VR_HANDS='1'; $env:SM2VR_HANDS_CALIBRATION=Join-Path $config 'index_hands_accepted.calibration'
        if ($InvalidateArmPoseCandidate) { $env:SM2VR_INVALIDATE_ARM_POSE='1' }
        if ($ResolveArmPoseCandidate) { $env:SM2VR_RESOLVE_ARM_POSE='1' }
        # Diagnostic A/B for near-body mesh distortion; keep every other control unchanged.
        if (-not $KeepHeadVisible) { $env:SM2VR_HIDE_HEAD='1' }
        # Photo mode freezes the player's arm and finger pose (docs/VIEWER_MODE.md); this opts out.
        if ($PhotoNativePose) { $env:SM2VR_PHOTO_NATIVE_POSE='1' }
        # Thumbs-up (docs/THUMBS_UP_2026-10-26.md): play by default; photo = only up to the photo freeze; off = live curls only.
        if ($ThumbsUp -ne 'play') { $env:SM2VR_THUMBS_UP=$ThumbsUp }
        # Melee hit check (docs/MELEE_SWEEP.md): replay the sword's swing path (default); animation =
        # the game's own sword; hand = the sword at the hand (before 28 Oct). Live: melee_sweep.txt beside shading.txt.
        if ($MeleeSweep -ne 'replay') { $env:SM2VR_MELEE_SWEEP=$MeleeSweep }
        Set-Content -LiteralPath (Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'melee_sweep.txt') -Value $MeleeSweep -Encoding ascii
        # Game rumble on the controllers (docs/HAPTICS.md), OpenXR only. On by default since
        # 22 October; -NoHaptics opts out.
        if (-not $NoHaptics) { $env:SM2VR_HAPTICS='1'; $env:SM2VR_HAPTICS_GAIN=$HapticsGain.ToString([Globalization.CultureInfo]::InvariantCulture) }
        if ($uprightTorsoOn) { $env:SM2VR_UPRIGHT_TORSO='1'; if ($NoUprightTwist) { $env:SM2VR_UPRIGHT_TWIST='0' } }
        if ($BodyPlacementCandidate) {
            $env:SM2VR_BODY_RENDER_BACK_METRES='0.2'
            $env:SM2VR_BODY_RENDER_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'body_placement.txt'
            Set-Content -LiteralPath $env:SM2VR_BODY_RENDER_CONTROL -Value '0.2' -Encoding ascii
        }
        if ($SecondCameraId -ge 0) { $env:SM2VR_SECOND_CAMERA_ID="$SecondCameraId" }
        if ($EyeSourceControl) {
            $env:SM2VR_EYE_SOURCE_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'eye_source.txt'
            Set-Content -LiteralPath $env:SM2VR_EYE_SOURCE_CONTROL -Value 'native' -Encoding ascii
        }
        # Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md): LightList/LightFlip lines, lights entering and leaving each view's list.
        if ($LightListLog) { $env:SM2VR_DIAG_LIGHT_LIST_LOG='1' }
        if ($LightListCopyControl) {
            # Prototype (docs/RIGHT_EYE_FLICKER_2026-10-28.md): copy_lights.txt native|copy (view 1 uses view 0's light list).
            $env:SM2VR_DIAG_LIGHT_LIST_COPY_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'copy_lights.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_LIGHT_LIST_COPY_CONTROL -Value 'native' -Encoding ascii
        }
        if ($LightCapSortControl) {
            # Candidate fix (docs/RIGHT_EYE_FLICKER_2026-10-28.md): cap_sort.txt native|distance (over 240 lights keep the nearest, not the largest pixel counts).
            $env:SM2VR_DIAG_LIGHT_CAP_SORT_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'cap_sort.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_LIGHT_CAP_SORT_CONTROL -Value 'native' -Encoding ascii
        }
        if ($LightClampControl) {
            # Candidate fix (docs/RIGHT_EYE_FLICKER_2026-10-28.md): light_clamp.txt native|clamp (passing occlusion results read as 4).
            $env:SM2VR_DIAG_LIGHT_CLAMP_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'light_clamp.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_LIGHT_CLAMP_CONTROL -Value 'native' -Encoding ascii
        }
        if ($SecondCameraPlainTestControl) {
            # Candidate fix (docs/RIGHT_EYE_FLICKER_2026-10-28.md): plain_test.txt native|plain (camera 1's light queries skip the cached-shadow test).
            $env:SM2VR_DIAG_SECOND_CAMERA_PLAIN_TEST_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'plain_test.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_SECOND_CAMERA_PLAIN_TEST_CONTROL -Value 'native' -Encoding ascii
        }
        if ($FarLightCacheControl) {
            # Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md): far_light.txt native|off (disables the far-light shadow cache).
            $env:SM2VR_DIAG_FAR_LIGHT_CACHE_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'far_light.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_FAR_LIGHT_CACHE_CONTROL -Value 'native' -Encoding ascii
        }
        if ($HarvestOnceControl) {
            # Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md): harvest.txt native|once (slot 1's Scene skips the occlusion harvest).
            $env:SM2VR_DIAG_HARVEST_ONCE_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'harvest.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_HARVEST_ONCE_CONTROL -Value 'native' -Encoding ascii
        }
        if ($LightHoldControl) {
            # Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md): light_hold.txt = frames to hold a light's last visible occlusion result, 0 off.
            $env:SM2VR_DIAG_LIGHT_HOLD_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'light_hold.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_LIGHT_HOLD_CONTROL -Value '0' -Encoding ascii
        }
        if ($AsyncLightingControl) {
            # Diagnostic (docs/RIGHT_EYE_FLICKER_2026-10-28.md): async_lighting.txt native|graphics|no_async.
            $env:SM2VR_DIAG_ASYNC_LIGHTING_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'async_lighting.txt'
            Set-Content -LiteralPath $env:SM2VR_DIAG_ASYNC_LIGHTING_CONTROL -Value 'native' -Encoding ascii
        }
        if ($PlayerPartVisibilityCandidate) {
            $env:SM2VR_PLAYER_PART_VISIBILITY_CONTROL=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'player_parts.txt'
            Set-Content -LiteralPath $env:SM2VR_PLAYER_PART_VISIBILITY_CONTROL -Value 'native' -Encoding ascii
        }
        $env:SM2VR_BODY_FOLLOW='1'
        $env:SM2VR_WEAPON_AIM='1'; $env:SM2VR_WEAPON_CONFIG=Join-Path $config 'index_weapon.calibration'
        # Analog chainsword rev: right trigger with the pistol/chainsword out sets teeth speed and
        # rev volume. On by default since 20 October; -NoChainswordRev opts out.
        if (-not $NoChainswordRev) { $env:SM2VR_CHAINSAW_REV='1' }
        if ($nativeHudOn) { $env:SM2VR_NATIVE_HUD='1' }
        # The game picks interaction targets from its own camera (docs/INTERACTION.md); steer it to the gaze.
        # On by default since 21 October; -NoGazeCamera opts out.
        if (-not $NoGazeCamera) { $env:SM2VR_GAZE_CAMERA='1'; $env:SM2VR_GAZE_CAMERA_CONFIG=Join-Path $config 'index_gaze_camera.calibration' }
        # Guard guide (docs/GUARD_GUIDE.md): a ghost bar at the left-hand guard pose while the sword is
        # out, coloured by the guard's state. Shown only when the guard runs (-Gestures). Opt-in with
        # -GuardGuide since 23 October (Joe: remove the parry indicator); enabled=0 in its config hides it too.
        if ($GuardGuide) { $env:SM2VR_GUARD_GUIDE='1'; $env:SM2VR_GUARD_GUIDE_CONFIG=Join-Path $config 'index_guard_guide.calibration' }
        # Confirmed sword loadout only: released trigger = light; held = power.
        $env:SM2VR_SWING_MELEE='1'; $env:SM2VR_SWING_CONFIG=Join-Path $config 'index_swing.calibration'
        $env:SM2VR_HOLSTERS='1'; $env:SM2VR_HOLSTER_CONFIG=Join-Path $config 'index_holster.calibration'
        # Holster D-pad bindings and the squad-ping unbinding (scripts/build_vr_controls.py).
        $pak=Join-Path $repo 'build\vr_controls\SM2VR_FirstPerson_Test.pak'
        if (-not $PrintEnv -and -not (Test-Path -LiteralPath $pak)) { throw "Controls pack missing: $pak (build it with scripts\build_vr_controls.py)" }
        if (-not $PrintEnv) {
            . (Join-Path $repo 'installer\app\sm2-install.ps1')
            if (-not (Test-Sm2PackLayout $pak)) { throw "Controls pack is out of date for this DLL: $pak (rebuild it with scripts\build_vr_controls.py)" }
        }
    }
    # Benchmark only: a copy of the controls pack with post-effect presets switched off (scripts\build_effects_bench.py).
    if ($EffectsPack) {
        if (-not $Controls) { throw '-EffectsPack is built from the controls pack; add -Controls.' }
        $pak=(Resolve-Path -LiteralPath $EffectsPack -ErrorAction Stop).Path
    }
    if ($Gestures) {
        $env:SM2VR_GUARD='1'; $env:SM2VR_GUARD_CONFIG=Join-Path $config 'index_guard.calibration'
        # Left-arm push: an open left hand thrust straight out sends the native held strike (knockback).
        $env:SM2VR_LEFT_PUSH='1'
        $env:SM2VR_GRENADE_GESTURE='1'; $env:SM2VR_GRENADE_CONFIG=Join-Path $config 'index_grenade.calibration'
        # Grenade aim (default; -NoGrenadeAim opts out): one native frag launch velocity per throw
        # follows the gaze/hand instead of the game camera, which never turns.
        if (-not $NoGrenadeAim) {
            $env:SM2VR_GRENADE_AIM='1'; $env:SM2VR_GRENADE_NATIVE_PROBE='1'; $env:SM2VR_GRENADE_AIM_CONFIG=Join-Path $config 'index_grenade_aim.calibration'
            if ($GrenadeVelocityProbe) { $env:SM2VR_GRENADE_VELOCITY_SAMPLES='1' }
        }
    }
    if ($TestDriver) {
        $driverRoot=Join-Path $repo 'build\test_driver'
        $driverDirectory=Join-Path $driverRoot ('session-'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
        New-Item -ItemType Directory -Path $driverDirectory -Force | Out-Null
        $env:SM2VR_TEST_DRIVER=$driverDirectory
        if ($ScriptedHead) { $env:SM2VR_TEST_HEAD='1' }
        @{directory=$driverDirectory;scripted_head=[bool]$ScriptedHead} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $driverRoot 'current.json')
        $Trace=$true
        "test driver mailbox: $driverDirectory (exclusive virtual controllers)"
    }
    if ($Trace) { $env:SM2VR_TRACE='1' }
    if ($DesktopWindow) { $env:SM2VR_DESKTOP_WINDOW=$DesktopWindow }
    if ($OutputSize) { $env:SM2VR_OUTPUT_SIZE=$OutputSize }
    if ($OutputSizeMode) { $env:SM2VR_OUTPUT_SIZE_MODE=$OutputSizeMode }
    $launchArgs = @($(if ($DirectLaunch) { '--startup --set /Config/WhiteListedVariables/enableModdersAutostartFlow true' }), $GameArgs) | Where-Object { $_ }
    $allGameArgs = $launchArgs -join ' '
    $captureOptions=@{};if($RenderDocCapture){$captureOptions.RenderDocCapture=$true}
    # Per-launch video overrides; run-modded.ps1 validates them and restores the saved settings (docs/RESOLUTION.md).
    if($Resolution){$captureOptions.Resolution=$Resolution};if($WindowMode){$captureOptions.WindowMode=$WindowMode};if($RenderResolution){$captureOptions.RenderResolution=$RenderResolution};if($Antialiasing){$captureOptions.Antialiasing=$Antialiasing}
    if($OutputSize){$captureOptions.OutputSize=$OutputSize}
    # -EnvOff 'SM2VR_STEREO,SM2VR_ORDINARY_NATIVE_CREATE' (diagnostic bisection): those variables are removed
    # after everything above set them, so one feature can be taken out of an otherwise normal launch.
    $envOffNames=@()
    foreach ($name in ($EnvOff -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        if ($name -notmatch '^SM2VR_[A-Z0-9_]+$') { throw "-EnvOff takes SM2VR_* names (got '$name')." }
        Remove-Item -LiteralPath ('Env:'+$name) -ErrorAction SilentlyContinue
        $envOffNames+=$name
    }
    # -EnvSet 'SM2VR_DIAG_NO_NATIVE_REFERENCE=1' (diagnostic): set those variables last. Everything not set
    # above is cleared at the start of this script, so a diagnostic switch can only get in this way.
    $envSetPairs=[ordered]@{}
    foreach ($pair in ($EnvSet -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        if ($pair -notmatch '^(SM2VR_[A-Z0-9_]+)=([^=]+)$') { throw "-EnvSet takes SM2VR_*=value pairs (got '$pair')." }
        Set-Item -LiteralPath ('Env:'+$Matches[1]) -Value $Matches[2]
        $envSetPairs[$Matches[1]]=$Matches[2]
    }
    # -PrintEnv: print the SM2VR_* environment this launch would use and stop (installer parity test).
    if ($PrintEnv) { $printed=[ordered]@{}; Get-ChildItem Env:SM2VR_* | Sort-Object Name | ForEach-Object { $printed[$_.Name]=$_.Value }; $printed | ConvertTo-Json; return }
    # What this launch runs beyond the player's defaults (docs/FLAGS.md): every non-shipped switch
    # away from its default, after the implied ones (-PerfCampaign, -TestDriver) took effect.
    # run-modded.ps1 prints the summary as "launch flags: ..." and saves launch-flags.json in the run folder.
    $flags=[ordered]@{verdict='';view='';player_options=@();diagnostics=@();dev_tools=@();env_set=$envSetPairs;env_off=@($envOffNames);summary=''}
    foreach ($parameter in $launcherParameters) {
        $name=$parameter.Name.VariablePath.UserPath
        $class=@($switchClass.Keys | Where-Object { $switchClass[$_] -contains $name })
        if ($class.Count -ne 1) { throw "Launcher switch -$name needs exactly one class in `$switchClass (docs/FLAGS.md)." }
        if ($class[0] -eq 'shipped' -or $unlistedSwitches -contains $name) { continue }
        $isSwitch=$parameter.StaticType -eq [switch]
        $value=Get-Variable -Name $name -ValueOnly
        $default=if ($parameter.DefaultValue) { $parameter.DefaultValue.SafeGetValue() } elseif ($isSwitch) { $false } else { '' }
        if ($isSwitch) { $value=[bool]$value }
        if ($value -eq $default) { continue }
        $text=if ($isSwitch) { "-$name" } elseif ("$value" -match '\s') { "-$name '$value'" } else { "-$name $value" }
        if ($class[0] -eq 'opt-out') { $flags.player_options+=$text } elseif ($class[0] -eq 'diagnostic') { $flags.diagnostics+=$text } else { $flags.dev_tools+=$text }
    }
    $flags.view=if ($Controls) { 'first person' } elseif ($ChaseCam) { 'chase' } else { 'stereo only' }
    if ($Controls -and -not $Gestures) { $flags.player_options=@('no -Gestures')+$flags.player_options }
    $offPlayer=$flags.diagnostics.Count+$flags.dev_tools.Count+$flags.env_set.Count+$flags.env_off.Count
    $flags.verdict=if ($flags.view -eq 'stereo only' -or $offPlayer) { 'NOT a player configuration' } elseif ($flags.player_options.Count) { 'player options' } else { 'player defaults' }
    $parts=@()
    if ($flags.verdict -eq 'player options') { $parts+=$flags.player_options -join ' ' }
    elseif ($flags.player_options.Count) { $parts+='player options '+($flags.player_options -join ' ') }
    if ($flags.diagnostics.Count) { $parts+='diagnostics '+($flags.diagnostics -join ' ') }
    if ($flags.dev_tools.Count) { $parts+='dev tools '+($flags.dev_tools -join ' ') }
    if ($flags.env_set.Count) { $parts+='-EnvSet '+(@($flags.env_set.Keys | ForEach-Object { "$_=$($flags.env_set[$_])" }) -join ',') }
    if ($flags.env_off.Count) { $parts+='-EnvOff '+($flags.env_off -join ',') }
    $flags.summary="$($flags.verdict) ($($flags.view))"+$(if ($parts.Count) { ': '+($parts -join '; ') } else { '' })
    $captureOptions.LaunchFlags=$flags
    # -AutoContinue presses Enter through the startup screens into the current save; otherwise no UI input at all.
    # -InGameKeys 'ESC@10,DOWN@12' (needs -AutoContinue): keys timed from the first stereo pair (run-modded.ps1).
    if($AutoContinue){$captureOptions.AutoContinue=$true;if($InGameKeys){$captureOptions.InGameKeys=$InGameKeys}}elseif($InGameKeys){throw '-InGameKeys needs -AutoContinue.'}else{$captureOptions.NoUiAutomation=$true}
    & (Join-Path $PSScriptRoot 'run-modded.ps1') -Seconds $Seconds -Tag $Tag -DataPak $pak -GameArgs $allGameArgs @captureOptions
} finally {
    # The companion ends with the game and then waits for its samplers to write; never leave it behind.
    if ($companion -and -not $companion.HasExited) {
        $companionReceipt=Join-Path $env:SM2VR_ORDINARY_DUMP_DIR 'companion.json'
        $adopted=$false;try{$adopted=[bool]((Get-Content -LiteralPath $companionReceipt -Raw | ConvertFrom-Json).game_pid)}catch{}
        # A launch that never started the game leaves nothing to record: stop the companion at once.
        if (-not $adopted -or -not $companion.WaitForExit(180000)) { Stop-Process -Id $companion.Id -Force -ErrorAction SilentlyContinue; Write-Warning 'perf companion stopped by the launcher' }
    }
    if ($companion) {
        # Safety net: no logger of this run may outlive it (nvidia-smi -lms never exits by itself).
        $dump = $env:SM2VR_ORDINARY_DUMP_DIR
        Get-CimInstance Win32_Process -Filter "Name='nvidia-smi.exe' OR Name='PresentMon-2.6.0-x64.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $dump -and $_.CommandLine -and $_.CommandLine.Contains($dump) } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; Write-Warning "stopped leftover $($_.Name) $($_.ProcessId)" }
    }
    Get-ChildItem Env:SM2VR_* | Remove-Item
    foreach($name in $saved.Keys) { Set-Item -LiteralPath ('Env:'+$name) -Value $saved[$name] }
}
