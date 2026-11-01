# Play profile (installer/launch.json) -> the SM2VR_* environment the DLL reads at startup.
# This must produce exactly what scripts/run-ordinary-headset.ps1 sets for the same switches;
# tests/test_installer_profile.py compares the two through that script's -PrintEnv.

# Set for every stereo launch by run-ordinary-headset.ps1. Which of these play needs and which are
# diagnostics is still to be tested in the headset (docs/INSTALLER_PLAN.md, open question 3).
$Sm2OrdinaryFlags = @('OBSERVER','SECOND_CAMERA','PAIR_COLOUR','PAIR_RETENTION','PRIVATE_DLSS','NATIVE_PACKET','TEMPORAL_PROBE','NATIVE_CREATE','NATIVE_BLOOM','TAA','CONSECUTIVE','FAULT_POD','SHARED_GRADING','SHARED_SHARPEN_EXPOSURE','SKIP_LATE_EFFECT','DISABLE_EARLY_EFFECT','HEADSET')

function Read-Sm2Profile([string]$Path) {
    $p = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if ($p.schema -ne 1) { throw "Unsupported launch profile schema: $($p.schema)" }
    if ($p.runtime -cnotin @('OpenXR', 'OpenVR')) { throw "launch.json runtime must be OpenXR or OpenVR (got '$($p.runtime)')." }
    if ($p.gestures -and -not $p.controls) { throw 'launch.json: gestures needs controls.' }
    foreach ($name in 'upright_torso', 'compact_hud', 'keep_head_visible', 'photo_native_pose', 'guard_guide', 'haptics') {
        if ($p.$name -and -not $p.controls) { throw "launch.json: $name needs controls." }
    }
    # thumbs_up: play (default when absent), photo or off (docs/THUMBS_UP_2026-10-26.md).
    if ($null -ne $p.thumbs_up -and $p.thumbs_up -cnotin @('play', 'photo', 'off')) { throw "launch.json thumbs_up must be play, photo or off (got '$($p.thumbs_up)')." }
    if ($null -ne $p.thumbs_up -and $p.thumbs_up -ne 'play' -and -not $p.controls) { throw 'launch.json: thumbs_up needs controls.' }
    if ($p.haptics -and ([double]$p.haptics_gain -lt 0 -or [double]$p.haptics_gain -gt 2)) { throw 'launch.json: haptics_gain must be 0-2.' }
    if ($p.grenade_aim -and -not $p.gestures) { throw 'launch.json: grenade_aim needs gestures.' }
    if ([int]$p.snap_degrees -le 0 -or [int]$p.snap_degrees -gt 90) { throw 'launch.json: snap_degrees must be 1-90.' }
    return $p
}

# Returns @{ Env = ordered name->value; Files = ordered path->content } for one launch.
# The caller creates DumpDirectory, writes Files, then sets Env.
# -Chase: the game's own chase camera with a gamepad (run-ordinary-headset.ps1 -ChaseCam). The
# profile's runtime, shadows and trace still apply; its motion controls and gestures do not.
function ConvertTo-Sm2Launch {
    param([Parameter(Mandatory)]$Profile, [Parameter(Mandatory)][string]$ConfigDirectory, [Parameter(Mandatory)][string]$DumpDirectory, [switch]$Chase)
    $e = [ordered]@{}; $files = [ordered]@{}
    foreach ($name in $Sm2OrdinaryFlags) { $e['SM2VR_ORDINARY_' + $name] = '1' }
    if ($Profile.runtime -eq 'OpenXR') { $e['SM2VR_OPENXR'] = '1' } else { $e['SM2VR_OPENVR'] = '1' }
    $e['SM2VR_STEREO'] = '1'
    $e['SM2VR_CALIBRATION'] = Join-Path $ConfigDirectory 'index.calibration'
    $e['SM2VR_CAMERA_CONFIG'] = Join-Path $ConfigDirectory 'index_camera_modes.calibration'
    $e['SM2VR_ORDINARY_DUMP_DIR'] = $DumpDirectory
    $e['SM2VR_ORDINARY_SHADING_CONTROL'] = Join-Path $DumpDirectory 'shading.txt'
    $files[$e['SM2VR_ORDINARY_SHADING_CONTROL']] = $(if ($Profile.native_shadows) { 'light_union' } else { 'light_union_shadow_refresh' })
    # The right eye's view takes the HDR core's index-0 eye adaptation (NativeAdaptationMirror.h), as in the dev launcher.
    $e['SM2VR_ADAPTATION_MIRROR'] = '1'
    if ($Chase) {
        $e['SM2VR_CHASE_CAM'] = '1'
        # The game's HUD, prompt and markers in the headset, as in first person.
        if ($Profile.compact_hud) { $e['SM2VR_NATIVE_HUD'] = '1' }
    }
    if ($Profile.controls -and -not $Chase) {
        $e['SM2VR_CONTROLLERS'] = '1'
        $e['SM2VR_ACTION_MANIFEST'] = Join-Path $ConfigDirectory 'vr_actions.json'
        if ($Profile.runtime -eq 'OpenXR') { $e['SM2VR_OPENXR_GRIP_CALIBRATION'] = Join-Path $ConfigDirectory 'openxr_index_grip.calibration' }
        $e['SM2VR_SNAP_DEGREES'] = [string][int]$Profile.snap_degrees
        $e['SM2VR_HANDS'] = '1'
        $e['SM2VR_HANDS_CALIBRATION'] = Join-Path $ConfigDirectory 'index_hands_accepted.calibration'
        if (-not $Profile.keep_head_visible) { $e['SM2VR_HIDE_HEAD'] = '1' }
        # Photo mode freezes the player's arm and finger pose unless the game's own was chosen (docs/VIEWER_MODE.md).
        if ($Profile.photo_native_pose) { $e['SM2VR_PHOTO_NATIVE_POSE'] = '1' }
        # Thumbs-up: play unless the profile asks for photo-only or off.
        if ($null -ne $Profile.thumbs_up -and $Profile.thumbs_up -ne 'play') { $e['SM2VR_THUMBS_UP'] = [string]$Profile.thumbs_up }
        if ($Profile.upright_torso) { $e['SM2VR_UPRIGHT_TORSO'] = '1' }
        # Game rumble on the controllers (docs/HAPTICS.md; the DLL plays it on OpenXR only).
        if ($Profile.haptics) {
            $e['SM2VR_HAPTICS'] = '1'
            $e['SM2VR_HAPTICS_GAIN'] = ([double]$Profile.haptics_gain).ToString([Globalization.CultureInfo]::InvariantCulture)
        }
        $e['SM2VR_BODY_FOLLOW'] = '1'
        $e['SM2VR_WEAPON_AIM'] = '1'
        $e['SM2VR_WEAPON_CONFIG'] = Join-Path $ConfigDirectory 'index_weapon.calibration'
        if ($Profile.chainsword_rev) { $e['SM2VR_CHAINSAW_REV'] = '1' }
        if ($Profile.compact_hud) { $e['SM2VR_NATIVE_HUD'] = '1' }
        if ($Profile.gaze_camera) {
            $e['SM2VR_GAZE_CAMERA'] = '1'
            $e['SM2VR_GAZE_CAMERA_CONFIG'] = Join-Path $ConfigDirectory 'index_gaze_camera.calibration'
        }
        if ($Profile.guard_guide) {
            $e['SM2VR_GUARD_GUIDE'] = '1'
            $e['SM2VR_GUARD_GUIDE_CONFIG'] = Join-Path $ConfigDirectory 'index_guard_guide.calibration'
        }
        $e['SM2VR_SWING_MELEE'] = '1'
        $e['SM2VR_SWING_CONFIG'] = Join-Path $ConfigDirectory 'index_swing.calibration'
        $e['SM2VR_HOLSTERS'] = '1'
        $e['SM2VR_HOLSTER_CONFIG'] = Join-Path $ConfigDirectory 'index_holster.calibration'
    }
    if ($Profile.gestures -and -not $Chase) {
        $e['SM2VR_GUARD'] = '1'
        $e['SM2VR_GUARD_CONFIG'] = Join-Path $ConfigDirectory 'index_guard.calibration'
        $e['SM2VR_LEFT_PUSH'] = '1'
        $e['SM2VR_GRENADE_GESTURE'] = '1'
        $e['SM2VR_GRENADE_CONFIG'] = Join-Path $ConfigDirectory 'index_grenade.calibration'
        if ($Profile.grenade_aim) {
            $e['SM2VR_GRENADE_AIM'] = '1'
            $e['SM2VR_GRENADE_NATIVE_PROBE'] = '1'
            $e['SM2VR_GRENADE_AIM_CONFIG'] = Join-Path $ConfigDirectory 'index_grenade_aim.calibration'
        }
    }
    if ($Profile.trace) { $e['SM2VR_TRACE'] = '1' }
    return @{ Env = $e; Files = $files }
}
