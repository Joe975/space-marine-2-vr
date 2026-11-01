# Capture only a manually verified, stationary, warmed-up gameplay scene. No UI input.
param(
    [Parameter(Mandatory)][int]$GameProcessId,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$Label,
    [ValidateSet('tracking','copy','full')][string]$Mode='full',
    [int]$Seconds=45,
    [int]$WarmupSeconds=15
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$tool=Join-Path $root 'tools\presentmon\PresentMon-2.6.0-x64.exe'
# tools\ is ignored by git, so a worktree has none: use the main checkout's copy.
if(-not (Test-Path -LiteralPath $tool)){
    $common=git -C $root rev-parse --path-format=absolute --git-common-dir 2>$null
    if($common){$tool=Join-Path (Split-Path $common -Parent) 'tools\presentmon\PresentMon-2.6.0-x64.exe'}
}
if(-not (Test-Path -LiteralPath $tool)){throw "PresentMon 2.6.0 missing: $tool (see docs/PERFORMANCE.md, Tools and metrics)"}
if(-not [IO.Path]::IsPathRooted($OutputDirectory)) {throw 'Absolute output directory required'}
if($Label -notmatch '^[a-zA-Z0-9_-]+$') {throw 'Use a plain filename label'}
if($Seconds -lt 10 -or $Seconds -gt 120 -or $WarmupSeconds -lt 0 -or $WarmupSeconds -gt 60) {throw 'Capture/warmup outside bounds'}
$process=Get-Process -Id $GameProcessId -ErrorAction Stop
if($process.ProcessName -ne 'Warhammer 40000 Space Marine 2 - Retail') {throw 'Target must be the retail game process'}
if((Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash -ne 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af') {throw 'PresentMon version/hash mismatch'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$csv=Join-Path $output ($Label+'.csv')
if(Test-Path -LiteralPath $csv) {throw 'Refusing to overwrite an existing capture'}
$control=Join-Path $output 'mode.txt'
$temporary=Join-Path $output 'mode.pending'
Set-Content -LiteralPath $temporary -Value $Mode -Encoding ascii
Move-Item -LiteralPath $temporary -Destination $control -Force
$metadata=[ordered]@{label=$Label; mode=$Mode; process_id=$GameProcessId; seconds=$Seconds; warmup_seconds=$WarmupSeconds;
    utc_start=(Get-Date).ToUniversalTime().ToString('o'); commit=(& git -C $root rev-parse HEAD);
    tool='PresentMon 2.6.0'; cpu=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name);
    gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion); desktop_present_only=$true}
$installedDll=Join-Path (Split-Path $process.Path -Parent) 'dxgi.dll'
$metadata.installed_dll_sha256=if(Test-Path -LiteralPath $installedDll) {(Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash} else {$null}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output ($Label+'.metadata.json')) -Encoding utf8
& $tool --process_id $GameProcessId --output_file $csv --qpc_time_ms --v2_metrics --no_console_stats --no_track_input --session_name ('SM2VR_'+$GameProcessId) --delay $WarmupSeconds --timed $Seconds --terminate_after_timed
if($LASTEXITCODE -ne 0) {throw "PresentMon exited $LASTEXITCODE"}
if(-not (Test-Path -LiteralPath $csv)) {throw 'PresentMon produced no capture'}
Write-Output "Captured $Label ($Mode) to $csv"
