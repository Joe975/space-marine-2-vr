# Temporary, transactional video-setting overrides for one launch (docs/RESOLUTION.md).
#
# The game keeps the user's video choices as plain JSON in
#   %LOCALAPPDATA%\Saber\Space Marine 2\storage\user\<steam id>\MainLocal\config\platform_usr_settings.cfg
# (PlatformUserSettings.Video: renderResolutionPresetRef, displayModeRef, resolutionWidthRef, ...).
# The engine's own platform\config\video.cfg and video_quality.cfg are opaque SSF1 containers
# (MD5-checked, not decodable here); they are never edited, only backed up and restored
# byte-for-byte, because the game rewrites them on every run.
#
# Install-Sm2VideoSettings backs up all three files into the run folder, writes a pending
# marker, then edits only the requested JSON entries. Restore-Sm2VideoSettings puts the
# original bytes back and removes the marker. While a marker exists every launch refuses,
# so a crash or a refused cleanup can never leave the user's settings silently changed.

$script:Sm2RenderResolutionUids = [ordered]@{
    Native = 'NATIVE'; Quality = 'QUALITY'; Balanced = 'BALANCED'; Performance = 'PERFORMANCE'
    UltraPerformance = 'ULTRA_PERFORMANCE'; Dynamic = 'DYNAMIC_RESOLUTION'
}
$script:Sm2WindowModeUids = [ordered]@{ Windowed = 'WINDOWED'; Borderless = 'BORDERLESS'; Fullscreen = 'FULL_SCREEN' }
$script:Sm2AntialiasingUids = [ordered]@{ TAA = 'TAA'; FSR2 = 'FSR2'; DLSS = 'DLSS'; MFSR = 'MFSR' }
# Eye copies, heap pools and readbacks in the mod accept at most 4096x2160 (GpuInlineCopy.cpp).
$script:Sm2ResolutionLimits = @{ MinWidth = 1280; MaxWidth = 3840; MinHeight = 720; MaxHeight = 2160 }

function Get-Sm2VideoSettingsPendingPath {
    Join-Path (Join-Path $env:LOCALAPPDATA 'sm2vr') 'video-settings-pending.json'
}

function Resolve-Sm2Choice([string]$Value, $Table, [string]$Option) {
    foreach ($key in $Table.Keys) { if ($key -eq $Value) { return $key } }
    throw "$Option must be one of: $($Table.Keys -join ', ') (got '$Value')."
}

# Validates launcher options. Returns $null when no override was requested.
# -OutputSize is applied by the mod (SM2VR_OUTPUT_SIZE, EngineOutputSize.h), not through the JSON;
# it still backs up and restores the settings files, in case the game saves the size it ran at.
function New-Sm2VideoOverride([string]$Resolution = '', [string]$WindowMode = '', [string]$RenderResolution = '', [string]$OutputSize = '', [string]$Antialiasing = '') {
    if (-not $Resolution -and -not $WindowMode -and -not $RenderResolution -and -not $OutputSize -and -not $Antialiasing) { return $null }
    $override = [ordered]@{}
    if ($OutputSize) {
        # Same rule as engine_output_size::parse: even, 640x360 to 4096x2160.
        if ($OutputSize -cnotmatch '^([1-9][0-9]{2,3})x([1-9][0-9]{2,3})$') { throw "-OutputSize must look like 1944x2160 (got '$OutputSize')." }
        $ow = [int]$Matches[1]; $oh = [int]$Matches[2]
        if ($ow -lt 640 -or $ow -gt 4096 -or $oh -lt 360 -or $oh -gt 2160 -or $ow % 2 -or $oh % 2) {
            throw "-OutputSize must be even and between 640x360 and 4096x2160 (got $OutputSize)."
        }
        if ($Resolution) { throw '-OutputSize cannot be combined with -Resolution (the mod sets the output size itself).' }
        $override.OutputWidth = $ow; $override.OutputHeight = $oh
    }
    if ($WindowMode) {
        $mode = Resolve-Sm2Choice $WindowMode $script:Sm2WindowModeUids '-WindowMode'
        $override.WindowMode = $mode; $override.WindowModeUid = $script:Sm2WindowModeUids[$mode]
    }
    if ($Resolution) {
        if ($Resolution -cnotmatch '^([1-9][0-9]{2,3})x([1-9][0-9]{2,3})$') { throw "-Resolution must look like 2560x1440 (got '$Resolution')." }
        $width = [int]$Matches[1]; $height = [int]$Matches[2]; $l = $script:Sm2ResolutionLimits
        if ($width -lt $l.MinWidth -or $width -gt $l.MaxWidth -or $height -lt $l.MinHeight -or $height -gt $l.MaxHeight) {
            throw "-Resolution must be between $($l.MinWidth)x$($l.MinHeight) and $($l.MaxWidth)x$($l.MaxHeight) (got $Resolution)."
        }
        if ($width % 2 -or $height % 2) { throw "-Resolution needs even dimensions (got $Resolution)." }
        # Borderless locks the output size to the desktop (video menu, 19 October probe).
        if ($override.WindowMode -notin @('Windowed', 'Fullscreen')) {
            throw '-Resolution needs -WindowMode Windowed or Fullscreen; borderless always renders at the desktop size.'
        }
        $override.Width = $width; $override.Height = $height
    }
    if ($RenderResolution) {
        $preset = Resolve-Sm2Choice $RenderResolution $script:Sm2RenderResolutionUids '-RenderResolution'
        $override.RenderResolution = $preset; $override.RenderResolutionUid = $script:Sm2RenderResolutionUids[$preset]
    }
    if ($Antialiasing) {
        $method = Resolve-Sm2Choice $Antialiasing $script:Sm2AntialiasingUids '-Antialiasing'
        $override.Antialiasing = $method; $override.AntialiasingUid = $script:Sm2AntialiasingUids[$method]
    }
    [pscustomobject]$override
}

# Pure text edit: replaces only the requested PlatformUserSettings.Video entries, leaving
# every other byte of the game's JSON unchanged. Fails closed on any unexpected layout.
function Set-Sm2VideoSettingsJson([string]$Text, $Override) {
    try { $parsed = $Text | ConvertFrom-Json } catch { throw "platform_usr_settings.cfg is not valid JSON: $($_.Exception.Message)" }
    $video = $parsed.PlatformUserSettings.Video
    if ($null -eq $video) { throw 'platform_usr_settings.cfg has no PlatformUserSettings.Video section.' }
    $edits = [ordered]@{}
    if ($Override.WindowModeUid) { $edits.displayModeRef = '{"json_version": 1, "uid": "' + $Override.WindowModeUid + '"}' }
    if ($Override.Width) {
        $edits.resolutionWidthRef = '{"json_version": 1, "id": ' + $Override.Width + '}'
        $edits.resolutionHeightRef = '{"json_version": 1, "id": ' + $Override.Height + '}'
    }
    if ($Override.RenderResolutionUid) { $edits.renderResolutionPresetRef = '{"json_version": 1, "uid": "' + $Override.RenderResolutionUid + '"}' }
    if ($Override.AntialiasingUid) { $edits.antialiasingMethodRef = '{"json_version": 1, "uid": "' + $Override.AntialiasingUid + '"}' }
    foreach ($key in $edits.Keys) {
        if (-not ($video.PSObject.Properties.Name -contains $key)) { throw "PlatformUserSettings.Video.$key is missing; unknown settings layout." }
        $pattern = '("' + $key + '"\s*:\s*)(null|\{[^{}]*\})'
        $found = [regex]::Matches($Text, $pattern)
        if ($found.Count -ne 1) { throw "Expected one $key entry with a flat value, found $($found.Count)." }
        $Text = [regex]::Replace($Text, $pattern, '${1}' + $edits[$key].Replace('$', '$$'))
    }
    # Re-parse and check every edited value round-trips as intended.
    $check = ($Text | ConvertFrom-Json).PlatformUserSettings.Video
    if ($Override.WindowModeUid -and $check.displayModeRef.uid -ne $Override.WindowModeUid) { throw 'Window mode edit did not round-trip.' }
    if ($Override.Width -and ($check.resolutionWidthRef.id -ne $Override.Width -or $check.resolutionHeightRef.id -ne $Override.Height)) { throw 'Resolution edit did not round-trip.' }
    if ($Override.RenderResolutionUid -and $check.renderResolutionPresetRef.uid -ne $Override.RenderResolutionUid) { throw 'Render resolution edit did not round-trip.' }
    if ($Override.AntialiasingUid -and $check.antialiasingMethodRef.uid -ne $Override.AntialiasingUid) { throw 'Antialiasing edit did not round-trip.' }
    $Text
}

function Get-Sm2VideoSettingsFiles([string]$StorageRoot) {
    $users = @(Get-ChildItem -LiteralPath (Join-Path $StorageRoot 'user') -Directory -ErrorAction SilentlyContinue |
        Where-Object { [IO.File]::Exists((Join-Path $_.FullName 'MainLocal\config\platform_usr_settings.cfg')) })
    if ($users.Count -ne 1) { throw "Expected exactly one user profile with platform_usr_settings.cfg under $StorageRoot\user, found $($users.Count)." }
    $files = @(
        (Join-Path $users[0].FullName 'MainLocal\config\platform_usr_settings.cfg'),
        (Join-Path $StorageRoot 'platform\config\video.cfg'),
        (Join-Path $StorageRoot 'platform\config\video_quality.cfg'))
    foreach ($file in $files) { if (-not [IO.File]::Exists($file)) { throw "Game settings file missing: $file" } }
    $files
}

# SHA-256 via .NET: Get-FileHash is a script function in Windows PowerShell 5.1 and is
# affected by the file-cmdlet mocks in the launcher tests.
function Get-Sm2Sha256([string]$Path = '', [byte[]]$Bytes = $null) {
    if ($null -eq $Bytes) { $Bytes = [IO.File]::ReadAllBytes($Path) }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-', '') } finally { $sha.Dispose() }
}

function Write-Sm2BytesAtomically([string]$Path, [byte[]]$Bytes) {
    # .NET file calls only: the launcher test fixtures mock the file cmdlets.
    $temporary = $Path + '.sm2vr-tmp'
    [IO.File]::WriteAllBytes($temporary, $Bytes)
    try { if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) } else { [IO.File]::Move($temporary, $Path) } }
    finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}

function Assert-Sm2NoPendingVideoSettings([string]$PendingPath = (Get-Sm2VideoSettingsPendingPath)) {
    if ([IO.File]::Exists($PendingPath)) {
        throw "A previous launch's video-setting override was not restored ($PendingPath). Close the game, then run scripts\restore-video-settings.ps1."
    }
}

function Install-Sm2VideoSettings($Override, [string]$StorageRoot, [string]$RunDirectory, [string]$PendingPath = (Get-Sm2VideoSettingsPendingPath)) {
    if ($null -eq $Override) { return $null }
    Assert-Sm2NoPendingVideoSettings $PendingPath
    if (-not [IO.Directory]::Exists($RunDirectory)) { throw 'Run directory missing' }
    $files = Get-Sm2VideoSettingsFiles $StorageRoot
    $json = $files[0]
    $originalBytes = [IO.File]::ReadAllBytes($json)
    $utf8 = New-Object Text.UTF8Encoding($false, $true)
    if ($originalBytes.Length -ge 3 -and $originalBytes[0] -eq 0xEF -and $originalBytes[1] -eq 0xBB -and $originalBytes[2] -eq 0xBF) { throw 'platform_usr_settings.cfg has an unexpected byte-order mark.' }
    $newText = Set-Sm2VideoSettingsJson ($utf8.GetString($originalBytes)) $Override
    $newBytes = $utf8.GetBytes($newText)

    $backupDirectory = Join-Path $RunDirectory 'video-settings-backup'
    New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null
    $entries = @()
    foreach ($file in $files) {
        $hash = (Get-Sm2Sha256 $file)
        $backup = Join-Path $backupDirectory ([IO.Path]::GetFileName($file))
        [IO.File]::Copy($file, $backup, $true)
        if ((Get-Sm2Sha256 $backup) -ne $hash) { throw "Backup of $file does not match the original." }
        $entries += [pscustomobject]@{ Path = $file; Backup = $backup; Sha256 = $hash }
    }
    $receipt = [pscustomobject]@{ PendingPath = $PendingPath; Requested = $Override; Files = $entries; CreatedUtc = [DateTime]::UtcNow.ToString('o') }
    New-Item -ItemType Directory -Force -Path (Split-Path $PendingPath -Parent) | Out-Null
    # The marker exists before the first edit, so an interrupted launch is always detected.
    [IO.File]::WriteAllText($PendingPath, ($receipt | ConvertTo-Json -Depth 4))
    try {
        [IO.File]::WriteAllText((Join-Path $RunDirectory 'video-settings.json'), ($receipt | ConvertTo-Json -Depth 4))
        Write-Sm2BytesAtomically $json $newBytes
        $expected = Get-Sm2Sha256 -Bytes $newBytes
        if ((Get-Sm2Sha256 $json) -ne $expected) { throw 'Edited platform_usr_settings.cfg does not match the intended content.' }
    } catch {
        $failure = $_
        if (-not (Restore-Sm2VideoSettings $receipt)) { Write-Warning "Video settings rollback failed; marker kept at $PendingPath" }
        throw $failure
    }
    $receipt
}

# Puts the original bytes back. Returns $true when every file matches its original and the
# marker is gone; otherwise keeps the marker (so later launches refuse) and returns $false.
function Restore-Sm2VideoSettings($Receipt) {
    if ($null -eq $Receipt) { return $true }
    $ok = $true
    foreach ($entry in $Receipt.Files) {
        try {
            if ([IO.File]::Exists($entry.Path) -and (Get-Sm2Sha256 $entry.Path) -eq $entry.Sha256) { continue }
            if (-not [IO.File]::Exists($entry.Backup) -or (Get-Sm2Sha256 $entry.Backup) -ne $entry.Sha256) {
                Write-Warning "Backup missing or changed, not restoring: $($entry.Backup)"; $ok = $false; continue
            }
            Write-Sm2BytesAtomically $entry.Path ([IO.File]::ReadAllBytes($entry.Backup))
            if ((Get-Sm2Sha256 $entry.Path) -ne $entry.Sha256) { Write-Warning "Restore did not stick: $($entry.Path)"; $ok = $false }
        } catch { Write-Warning "Restore failed for $($entry.Path): $_"; $ok = $false }
    }
    if ($ok -and $Receipt.PendingPath -and [IO.File]::Exists($Receipt.PendingPath)) { [IO.File]::Delete($Receipt.PendingPath) }
    $ok
}

function Restore-Sm2PendingVideoSettings([string]$PendingPath = (Get-Sm2VideoSettingsPendingPath)) {
    if (-not (Test-Path -LiteralPath $PendingPath)) { return $true }
    $receipt = Get-Content -LiteralPath $PendingPath -Raw | ConvertFrom-Json
    if ($receipt.PendingPath -ne $PendingPath) { throw "Pending marker names a different location: $($receipt.PendingPath)" }
    Restore-Sm2VideoSettings $receipt
}

# The mod logs "Sm2Probe: <reason>: swap chain WxH" at init, first present and every device
# reset; the last line is the output size the game settled on.
function Get-Sm2ObservedSwapChain([string]$LogPath) {
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return $null }
    $last = $null
    foreach ($m in (Select-String -LiteralPath $LogPath -Pattern 'Sm2Probe: ([a-z0-9 ]+): swap chain (\d+)x(\d+)')) {
        $g = $m.Matches[0].Groups
        $last = [pscustomobject]@{ Reason = $g[1].Value; Width = [int]$g[2].Value; Height = [int]$g[3].Value }
    }
    $last
}
