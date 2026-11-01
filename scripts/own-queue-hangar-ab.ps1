# Unattended hangar A/B of OpenXR submission options (docs/OPENXR_OWN_QUEUE.md, OPENXR_SUBMIT_THREAD.md):
# one launch per arm, in the order given, each driven to the hangar, then scripts\hangar-route.ps1 (pair
# rate, pose age, both-eye mirror recordings for the flicker measure), then closed.
# -Arms maps each arm letter to its extra launcher switches; -Order lists the launches.
# Usage: own-queue-hangar-ab.ps1 [-Order 'A,B,B,A'] [-Arms 'A=-OpenXrGameQueue -NoOpenXrSubmitThread;B='] [-Route ...] [-Extra '-PerfCampaign']
# -Route may hold hangar-route.ps1's eye:/shading: tokens (the launch's dump folder is passed as its
# -ControlDir) and step@n sizes; -Downscale is the route's default size (1/16).
# Table: scripts\summarize_hangar_route.py <abs build\drive\own_queue_ab_<Stamp>>.
param([string]$Order = 'A,B,B,A', [string]$Arms = 'A=-OpenXrGameQueue -NoOpenXrSubmitThread;B=',
    [string]$Route = 'still,fp,fp_back,enter,drag,fast,drag,fast,exit,still',
    [string]$Extra = '', [string]$Stamp = (Get-Date -Format 'yyyyMMdd_HHmm'), [int]$Downscale = 16)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$armFlags = @{}
foreach ($pair in $Arms.Split(';')) { $name, $value = $pair.Split('=', 2); $armFlags[$name.Trim()] = $value.Trim() }
$out = Join-Path $root "build\drive\own_queue_ab_$Stamp"
New-Item -ItemType Directory -Force -Path $out | Out-Null
"$Arms" | Out-File -Encoding ascii (Join-Path $out 'arms.txt')
$i = 0
foreach ($arm in $Order.Split(',')) {
    if (-not $armFlags.ContainsKey($arm)) { throw "arm '$arm' is not in -Arms" }
    $i++; $tag = "oq_$Stamp`_$i$arm"
    $flags = "-Controls -Gestures -TestDriver -ScriptedHead -Trace -Seconds 1500 -Tag $tag $Extra $($armFlags[$arm])"
    $dir = Join-Path $out "$i$arm"
    $attempt = 0
    do {
        $attempt++
        & (Join-Path $PSScriptRoot 'drive-game.ps1') -Launch $flags -Do 'continue,keys:ESC,wait:2,keys:ESC,wait:5' -RequireStereo -OutDir $dir
        $code = $LASTEXITCODE
        Write-Output "$i$arm launch attempt $attempt exit=$code"
        if ($code -ne 0) {
            Get-Process 'Warhammer 40000 Space Marine 2 - Retail' -ErrorAction SilentlyContinue | Stop-Process -Force
            Start-Sleep -Seconds 30
        }
    } while ($code -ne 0 -and $attempt -lt 3)
    if ($code -ne 0) { throw "$i$arm never reached stereo in the hangar" }
    $control = Get-ChildItem (Join-Path $root 'build\ordinary') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName $tag } | Where-Object { Test-Path $_ } | Select-Object -Last 1
    try { & (Join-Path $PSScriptRoot 'hangar-route.ps1') -OutDir $out -Route $Route -Name "$i$arm" -Downscale $Downscale -ControlDir "$control" }
    finally { Get-Process 'Warhammer 40000 Space Marine 2 - Retail' -ErrorAction SilentlyContinue | Stop-Process -Force }
    Start-Sleep -Seconds 40 # the launcher removes the mod files and saves the run folder
    $run = Get-ChildItem (Join-Path $root 'build\run') -Directory | Where-Object Name -like "*_$tag" | Sort-Object Name | Select-Object -Last 1
    "$i$arm,$($run.FullName)" | Out-File -Append -Encoding ascii (Join-Path $out 'runs.csv')
}
Write-Output "done: $out"
