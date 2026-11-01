# Flat/stereo A/B during a -PerfCampaign launch (docs/validation/campaign-perf-plan-2026-10-21.md).
# Writes the one-word control file the mod polls once a second: 'mono' holds the stereo producer in
# native mono (one native view per frame; the headset shows the flat panel), 'stereo' releases it.
# The mod records every second's state in cpu_trace.csv (stereo_hold), so the analysis finds the
# windows from the trace, not from this script's clock.
#   stereo-hold.ps1 -Mode mono
#   stereo-hold.ps1 -Cycle 'mono:45,stereo:60,mono:45,stereo:60'   # blocks; ends in stereo
# Without -Control the control file of the current -PerfCampaign launch is used
# (build\perf\current-campaign.json, written by run-ordinary-headset.ps1).
param([ValidateSet('mono','stereo')][string]$Mode='',[string]$Cycle='',[string]$Control='')
$ErrorActionPreference='Stop'
if((-not $Mode) -eq (-not $Cycle)){throw 'Give exactly one of -Mode or -Cycle.'}
if(-not $Control){
    $current=Join-Path (Split-Path $PSScriptRoot -Parent) 'build\perf\current-campaign.json'
    if(-not (Test-Path -LiteralPath $current)){throw "No -Control and no $current (start the game with -PerfCampaign)."}
    $Control=(Get-Content -LiteralPath $current -Raw | ConvertFrom-Json).stereo_hold_control
}
if(-not [IO.Path]::IsPathRooted($Control) -or -not (Test-Path -LiteralPath (Split-Path $Control -Parent) -PathType Container)){throw "Control must be an absolute path in an existing folder: $Control"}
$log=Join-Path (Split-Path $Control -Parent) 'stereo_hold_log.csv'
function Set-Hold([string]$value){
    # Atomic replace: the mod never reads a partial word.
    $temporary="$Control.pending"
    Set-Content -LiteralPath $temporary -Value $value -Encoding ascii -NoNewline
    Move-Item -LiteralPath $temporary -Destination $Control -Force
    if(-not (Test-Path -LiteralPath $log)){'utc,mode' | Set-Content -LiteralPath $log -Encoding ascii}
    "$((Get-Date).ToUniversalTime().ToString('o')),$value" | Add-Content -LiteralPath $log -Encoding ascii
    "$(Get-Date -Format 'HH:mm:ss') stereo hold: $value"
}
if($Mode){Set-Hold $Mode;return}
$steps=foreach($item in ($Cycle -split ',')){
    if($item.Trim() -notmatch '^(mono|stereo):([1-9][0-9]{0,3})$'){throw "Cycle steps look like mono:45 (got '$item')."}
    [pscustomobject]@{Mode=$Matches[1];Seconds=[int]$Matches[2]}
}
try{foreach($step in $steps){Set-Hold $step.Mode;Start-Sleep -Seconds $step.Seconds}}
finally{Set-Hold 'stereo'}
