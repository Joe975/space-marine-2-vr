# Exercises scripts\game-video-settings.ps1 against a fake game storage tree (tests/test_video_settings.py).
param([string]$Work)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts\game-video-settings.ps1')
function Assert($condition, $message) { if (-not $condition) { throw "FAIL: $message" } }
function Refuses([scriptblock]$block, [string]$expected) {
    try { & $block } catch { if ($expected -and $_.Exception.Message -notlike "*$expected*") { throw "FAIL: wrong refusal '$($_.Exception.Message)', wanted '$expected'" }; return $true }
    return $false
}
function Hash($path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

# Validation: strict, fail closed.
Assert ($null -eq (New-Sm2VideoOverride)) 'no options means no override'
$o = New-Sm2VideoOverride -Resolution '2560x1440' -WindowMode 'windowed' -RenderResolution 'Performance'
Assert ($o.Width -eq 2560 -and $o.Height -eq 1440 -and $o.WindowModeUid -eq 'WINDOWED' -and $o.WindowMode -eq 'Windowed' -and $o.RenderResolutionUid -eq 'PERFORMANCE') 'valid override'
Assert ((New-Sm2VideoOverride -RenderResolution UltraPerformance).RenderResolutionUid -eq 'ULTRA_PERFORMANCE') 'ultra performance uid'
Assert ((New-Sm2VideoOverride -RenderResolution Dynamic).RenderResolutionUid -eq 'DYNAMIC_RESOLUTION') 'dynamic uid'
Assert ((New-Sm2VideoOverride -WindowMode Fullscreen).WindowModeUid -eq 'FULL_SCREEN') 'fullscreen uid'
Assert ((New-Sm2VideoOverride -WindowMode Borderless).WindowModeUid -eq 'BORDERLESS') 'borderless uid'
Assert (Refuses { New-Sm2VideoOverride -Resolution '2560x1440' } 'needs -WindowMode') 'resolution without window mode'
Assert (Refuses { New-Sm2VideoOverride -Resolution '2560x1440' -WindowMode Borderless } 'borderless') 'resolution with borderless'
foreach ($bad in '2560*1440', '2560x1440x1', ' 2560x1440', '2560X1440', '4096x2160', '1024x768', '2561x1440', '0x0', '02560x1440', '2560x1440;rm') {
    Assert (Refuses { New-Sm2VideoOverride -Resolution $bad -WindowMode Windowed } '-Resolution') "bad resolution '$bad'"
}
$out = New-Sm2VideoOverride -OutputSize '1944x2160'
Assert ($out.OutputWidth -eq 1944 -and $out.OutputHeight -eq 2160 -and -not $out.Width -and -not $out.WindowModeUid) 'output size is mod-side only'
Assert ((New-Sm2VideoOverride -OutputSize '4096x2160' -RenderResolution Quality).RenderResolutionUid -eq 'QUALITY') 'output size with a preset'
foreach ($bad in '1945x2160', '1944x2162', '638x360', '4098x2160', '1944X2160', '01944x2160') {
    Assert (Refuses { New-Sm2VideoOverride -OutputSize $bad } '-OutputSize') "bad output size '$bad'"
}
Assert (Refuses { New-Sm2VideoOverride -OutputSize '1944x2160' -Resolution '2560x1440' -WindowMode Windowed } 'cannot be combined') 'output size with -Resolution'
foreach ($bad in 'Window', 'FULL_SCREEN', 'Exclusive', 'Windowed ') { Assert (Refuses { New-Sm2VideoOverride -WindowMode $bad } '-WindowMode') "bad mode '$bad'" }
foreach ($bad in 'DLAA', 'ULTRA_PERFORMANCE', 'Perf', '50%') { Assert (Refuses { New-Sm2VideoOverride -RenderResolution $bad } '-RenderResolution') "bad preset '$bad'" }

# JSON edit: only the requested entries change.
$json = @'
{
   "PlatformUserSettings": {
      "systemVersion": 130,
      "Video": {
         "json_version": 2,
         "renderResolutionPresetRef": {
            "json_version": 1,
            "uid": "ULTRA_PERFORMANCE"
         },
         "brightnessRef": {
            "json_version": 1,
            "value": -3.7252902984619141e-9
         },
         "motionBlurPresetNameRef": {
            "json_version": 1,
            "uid": "default_blur\\motion_blur_low"
         },
         "antialiasingMethodRef": {
            "json_version": 1,
            "uid": "DLSS"
         },
         "displayModeRef": null,
         "resolutionWidthRef": null,
         "resolutionHeightRef": null
      }
   }
}
'@ -replace "`r`n", "`n"
$edited = Set-Sm2VideoSettingsJson $json $o
$video = ($edited | ConvertFrom-Json).PlatformUserSettings.Video
Assert ($video.displayModeRef.uid -eq 'WINDOWED' -and $video.resolutionWidthRef.id -eq 2560 -and $video.resolutionHeightRef.id -eq 1440 -and $video.renderResolutionPresetRef.uid -eq 'PERFORMANCE') 'edited values'
Assert ($edited.Contains('"value": -3.7252902984619141e-9') -and $edited.Contains('"uid": "default_blur\\motion_blur_low"')) 'untouched entries are byte-identical'
Assert ((Set-Sm2VideoSettingsJson $json (New-Sm2VideoOverride -RenderResolution Quality)).Contains('"displayModeRef": null')) 'unrequested entries stay null'
Assert (((Set-Sm2VideoSettingsJson $json (New-Sm2VideoOverride -Antialiasing TAA)) | ConvertFrom-Json).PlatformUserSettings.Video.antialiasingMethodRef.uid -eq 'TAA') 'antialiasing edit'
Assert ((Set-Sm2VideoSettingsJson $json (New-Sm2VideoOverride -Antialiasing TAA)).Contains('"uid": "ULTRA_PERFORMANCE"')) 'antialiasing leaves the render preset'
Assert (Refuses { New-Sm2VideoOverride -Antialiasing Off } '-Antialiasing') 'bad antialiasing method'
Assert (Refuses { Set-Sm2VideoSettingsJson '{"PlatformUserSettings":{}}' $o } 'no PlatformUserSettings.Video') 'missing Video section'
Assert (Refuses { Set-Sm2VideoSettingsJson ($json -replace '"displayModeRef": null,', '') $o } 'displayModeRef is missing') 'missing key'
Assert (Refuses { Set-Sm2VideoSettingsJson 'not json' $o } 'not valid JSON') 'invalid JSON'
Assert (Refuses { Set-Sm2VideoSettingsJson ($json -replace '"displayModeRef": null', '"displayModeRef": {"a": {"b": 1}}') $o } 'flat value') 'nested value refused'

# Fake storage tree.
$storage = Join-Path $Work 'storage'; $config = Join-Path $storage 'platform\config'; $user = Join-Path $storage 'user\7656\MainLocal\config'
New-Item -ItemType Directory -Force -Path $config, $user | Out-Null
$usr = Join-Path $user 'platform_usr_settings.cfg'; $vid = Join-Path $config 'video.cfg'; $vq = Join-Path $config 'video_quality.cfg'
[IO.File]::WriteAllBytes($usr, (New-Object Text.UTF8Encoding($false)).GetBytes($json))
[IO.File]::WriteAllBytes($vid, [byte[]](0x53, 0x53, 0x46, 0x31, 1, 2, 3)); [IO.File]::WriteAllBytes($vq, [byte[]](0x53, 0x53, 0x46, 0x31, 9))
$original = @{ $usr = (Hash $usr); $vid = (Hash $vid); $vq = (Hash $vq) }
$run = Join-Path $Work 'run'; New-Item -ItemType Directory -Force -Path $run | Out-Null
$pending = Join-Path $Work 'local\sm2vr\video-settings-pending.json'

Assert ($null -eq (Install-Sm2VideoSettings $null $storage $run $pending)) 'no override installs nothing'
Assert (-not (Test-Path -LiteralPath $pending)) 'no override, no marker'
$receipt = Install-Sm2VideoSettings $o $storage $run $pending
Assert (Test-Path -LiteralPath $pending) 'marker written'
Assert (Test-Path -LiteralPath (Join-Path $run 'video-settings.json')) 'run receipt written'
Assert ((Get-Content -LiteralPath $usr -Raw | ConvertFrom-Json).PlatformUserSettings.Video.displayModeRef.uid -eq 'WINDOWED') 'override applied'
Assert ((Hash $vid) -eq $original[$vid]) 'engine files are not edited'
Assert (-not (Test-Path -LiteralPath ($usr + '.sm2vr-tmp'))) 'no temporary file left'
Assert (Refuses { Assert-Sm2NoPendingVideoSettings $pending } 'restore-video-settings.ps1') 'pending marker blocks launches'
Assert (Refuses { Install-Sm2VideoSettings $o $storage $run $pending } 'not restored') 'second install refused while pending'
# The game rewrites all three files during a run.
[IO.File]::WriteAllText($usr, '{"rewritten": true}'); [IO.File]::WriteAllBytes($vid, [byte[]](7, 7)); [IO.File]::Delete($vq)
Assert (Restore-Sm2VideoSettings $receipt) 'restore succeeds'
foreach ($f in $usr, $vid, $vq) { Assert ((Hash $f) -eq $original[$f]) "restored byte-for-byte: $f" }
Assert (-not (Test-Path -LiteralPath $pending)) 'marker removed after restore'
Assert (Restore-Sm2VideoSettings $receipt) 'restore is idempotent'

# Output size only: the JSON stays byte-identical, but all three files are still protected.
$runOut = Join-Path $Work 'run_out'; New-Item -ItemType Directory -Force -Path $runOut | Out-Null
$receiptOut = Install-Sm2VideoSettings (New-Sm2VideoOverride -OutputSize '1944x2160') $storage $runOut $pending
Assert ((Hash $usr) -eq $original[$usr]) 'output-size launch leaves the JSON unchanged'
Assert (Test-Path -LiteralPath $pending) 'output-size launch still writes the marker'
[IO.File]::WriteAllBytes($vid, [byte[]](9, 9, 9))
Assert (Restore-Sm2VideoSettings $receiptOut) 'output-size restore'
Assert ((Hash $vid) -eq $original[$vid] -and -not (Test-Path -LiteralPath $pending)) 'engine file restored after an output-size launch'

# Crash recovery from the marker alone; a damaged backup keeps the marker.
$run2 = Join-Path $Work 'run2'; New-Item -ItemType Directory -Force -Path $run2 | Out-Null
$receipt2 = Install-Sm2VideoSettings (New-Sm2VideoOverride -RenderResolution Quality) $storage $run2 $pending
[IO.File]::WriteAllText($usr, '{"rewritten": true}')
[IO.File]::WriteAllText((Join-Path $run2 'video-settings-backup\platform_usr_settings.cfg'), 'damaged')
Assert (-not (Restore-Sm2PendingVideoSettings $pending)) 'damaged backup is reported'
Assert (Test-Path -LiteralPath $pending) 'marker kept when restore is incomplete'
[IO.File]::WriteAllBytes((Join-Path $run2 'video-settings-backup\platform_usr_settings.cfg'), (New-Object Text.UTF8Encoding($false)).GetBytes($json))
Assert (Restore-Sm2PendingVideoSettings $pending) 'marker-driven restore'
Assert ((Hash $usr) -eq $original[$usr] -and -not (Test-Path -LiteralPath $pending)) 'restored from marker'

# Layout refusals leave everything untouched and no marker.
$run3 = Join-Path $Work 'run3'; New-Item -ItemType Directory -Force -Path $run3 | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $storage 'user\9999\MainLocal\config') | Out-Null
Copy-Item -LiteralPath $usr -Destination (Join-Path $storage 'user\9999\MainLocal\config\platform_usr_settings.cfg')
Assert (Refuses { Install-Sm2VideoSettings $o $storage $run3 $pending } 'exactly one user profile') 'ambiguous profile refused'
Remove-Item -LiteralPath (Join-Path $storage 'user\9999') -Recurse -Force
[IO.File]::WriteAllText($usr, '{"PlatformUserSettings": {"Video": {"displayModeRef": null}}}')
$before = Hash $usr
Assert (Refuses { Install-Sm2VideoSettings $o $storage $run3 $pending } 'resolutionWidthRef is missing') 'unknown layout refused'
Assert ((Hash $usr) -eq $before -and -not (Test-Path -LiteralPath $pending)) 'refused install changes nothing'
[IO.File]::Delete($vq)
Assert (Refuses { Install-Sm2VideoSettings (New-Sm2VideoOverride -WindowMode Windowed) $storage $run3 $pending } 'missing') 'missing engine file refused'

# Observed swap chain from the mod log: last line wins.
$log = Join-Path $Work 'sm2vr.log'
Set-Content -LiteralPath $log -Value @(
    '[20:13:56.368] [I] [tid:1] Sm2Probe: d3d thread init: swap chain 3840x2160 format=28 buffers=3',
    '[20:14:01.043] [I] [tid:1] Sm2Probe: first present: swap chain 3840x2160 format=28 buffers=3',
    '[20:14:09.000] [I] [tid:1] Sm2Probe: device reset: swap chain 2560x1440 format=28 buffers=3')
$seen = Get-Sm2ObservedSwapChain $log
Assert ($seen.Width -eq 2560 -and $seen.Height -eq 1440 -and $seen.Reason -eq 'device reset') 'last swap chain reported'
Assert ($null -eq (Get-Sm2ObservedSwapChain (Join-Path $Work 'missing.log'))) 'missing log'
Set-Content -LiteralPath $log -Value '[20:13:56.368] [I] [tid:1] Sm2Probe: d3d thread init: swap chain 1920x1080 format=28'
Assert ((Get-Sm2ObservedSwapChain $log).Reason -eq 'd3d thread init') 'reason with digits'
'video settings fixture passed'
