# Campaign profiling companion (docs/validation/campaign-perf-plan-2026-10-21.md). Started hidden by
# run-ordinary-headset.ps1 -PerfCampaign just before the game launches. It never touches the game or
# its files. It waits for the retail game process that starts after it, then records into
# -OutputDirectory (the run's ordinary dump folder, next to cpu_trace.csv) until that process exits:
#   presentmon.csv        PresentMon 2.6.0: per-frame present interval, GPU busy, CPU busy/wait
#   gpu.csv               nvidia-smi once a second: GPU utilization and clocks (cross-check)
#   sampler_render.csv    native_thread_sampler snapshot stacks of the render (dispatch) thread
#   sampler_simulation.csv  ... and of the simulation thread, started once stereo pairs flow
#   companion.json        receipt: tools, hashes, PIDs, thread IDs, exit codes, times
# Thread sampling suspends each sampled thread for about 0.05 ms per sample (measured per sample in
# suspended_ms); at the default 20 Hz that is about 0.1% of each thread's time.
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(60,7200)][int]$MaxSeconds=5400,
    [ValidateRange(10,1800)][int]$WaitForGameSeconds=900,
    [ValidateRange(0,50)][int]$SampleHz=20,
    [ValidateRange(10,1000)][int]$SampleSeconds=900,
    [switch]$NoPresentMon,
    [switch]$NoGpuLog
)
$ErrorActionPreference='Stop'
if(-not [IO.Path]::IsPathRooted($OutputDirectory) -or -not (Test-Path -LiteralPath $OutputDirectory -PathType Container)){throw 'Existing absolute output directory required'}
$root=Split-Path $PSScriptRoot -Parent
$exeName='Warhammer 40000 Space Marine 2 - Retail'
$presentMonHash='b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
$receiptPath=Join-Path $OutputDirectory 'companion.json'
# Windows PowerShell 5.1 with ErrorActionPreference Stop turns any stderr line of a native tool
# (logman, PresentMon) into a terminating error. Run them through Invoke-Native, which never throws:
# on 22 Oct such an error ended the cleanup early and left nvidia-smi running.
function Invoke-Native([string]$file,[string[]]$arguments){
    try{$output=& cmd.exe /c "`"$file`" $($arguments -join ' ') 2>nul";return @($output)}catch{return @()}
}
$receipt=[ordered]@{started_utc=(Get-Date).ToUniversalTime().ToString('o');output=$OutputDirectory;commit=(& git -C $root rev-parse HEAD 2>$null)
    game_pid=$null;presentmon=$null;gpu_log=$null;samplers=@();threads=$null;stopped_utc=$null;notes=@()}
function Save-Receipt { $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding utf8 }
# tools\ and build\ are ignored by git, so a worktree has neither: fall back to the main checkout.
function Find-RepoFile([string]$relative) {
    $here=Join-Path $root $relative
    if(Test-Path -LiteralPath $here){return $here}
    try{$common=git -C $root rev-parse --path-format=absolute --git-common-dir 2>$null;if($common){$main=Join-Path (Split-Path $common -Parent) $relative;if(Test-Path -LiteralPath $main){return $main}}}catch{}
    return $null
}
# The latest thread IDs of the render (native dispatch) and simulation (PCL simulation start) roles,
# and whether stereo pairs are flowing, from the tail of the live trace (flushed once a second).
# The simulation thread is the PCL simulation-end thread: SM2 starts each simulation frame in a job.
function Read-TraceThreads([string]$trace) {
    if(-not (Test-Path -LiteralPath $trace)){return $null}
    $stream=[IO.File]::Open($trace,'Open','Read','ReadWrite,Delete')
    try{
        $length=$stream.Length;$take=[Math]::Min($length,[long]4MB);$null=$stream.Seek($length-$take,'Begin')
        $buffer=New-Object byte[] $take;$read=$stream.Read($buffer,0,$take)
        $lines=[Text.Encoding]::ASCII.GetString($buffer,0,$read) -split "`n"
    }finally{$stream.Dispose()}
    $found=@{render=$null;simulation=$null;pairs=0}
    foreach($line in $lines){
        $f=$line.TrimEnd("`r") -split ','
        if($f.Count -lt 6){continue}
        if($f[1] -eq 'ordinary_dispatch_native_cpu'){$found.render=[int]$f[5]}
        elseif($f[1] -eq 'pcl_simulation_end'){$found.simulation=[int]$f[5]}  # simulation starts on rotating job workers
        elseif($f[1] -eq 'pair_submitted'){$found.pairs++}
    }
    return $found
}
$companionStart=Get-Date
$game=$null
$deadline=$companionStart.AddSeconds($WaitForGameSeconds)
while(-not $game -and (Get-Date) -lt $deadline){
    $game=Get-Process -Name $exeName -ErrorAction SilentlyContinue | Where-Object { try{$_.StartTime -ge $companionStart.AddSeconds(-5)}catch{$false} } | Select-Object -First 1
    if(-not $game){Start-Sleep -Milliseconds 500}
}
if(-not $game){$receipt.notes+='no game process started';$receipt.stopped_utc=(Get-Date).ToUniversalTime().ToString('o');Save-Receipt;exit 1}
$receipt.game_pid=$game.Id;Save-Receipt
$presentMon=$null;$gpuLog=$null;$samplers=@()
try{
    if(-not $NoPresentMon){
        $tool=Find-RepoFile 'tools\presentmon\PresentMon-2.6.0-x64.exe'
        if(-not $tool){$receipt.notes+='PresentMon missing (tools\presentmon); GPU busy will be unknown'}
        elseif((Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash -ne $presentMonHash){$receipt.notes+='PresentMon hash mismatch; not run'}
        else{
            # A PresentMon that was killed leaves its ETW session running, and the next capture then
            # loses events and writes no CSV (seen on 21 Oct). Stop leftovers of earlier runs first.
            $stale=@(Invoke-Native 'logman.exe' @('query','-ets') | ForEach-Object { if($_ -match '^(SM2VR_CAMPAIGN_\d+)\s'){$Matches[1]} })
            foreach($session in $stale){$null=Invoke-Native 'logman.exe' @('stop',$session,'-ets');$receipt.notes+="stopped stale ETW session $session"}
            $csv=Join-Path $OutputDirectory 'presentmon.csv'
            $arguments=@('--process_id',$game.Id,'--output_file',"`"$csv`"",'--qpc_time_ms','--v2_metrics','--no_console_stats','--no_track_input',
                '--terminate_on_proc_exit','--stop_existing_session','--session_name',"SM2VR_CAMPAIGN_$($game.Id)")
            $presentMon=Start-Process -FilePath $tool -ArgumentList $arguments -WindowStyle Hidden -PassThru `
                -RedirectStandardError (Join-Path $OutputDirectory 'presentmon.stderr.txt') -RedirectStandardOutput (Join-Path $OutputDirectory 'presentmon.stdout.txt')
            $null=$presentMon.Handle  # keeps ExitCode readable after exit
            $receipt.presentmon=[ordered]@{pid=$presentMon.Id;tool=$tool;sha256=$presentMonHash;csv=$csv;session="SM2VR_CAMPAIGN_$($game.Id)";exit_code=$null}
        }
    }
    if(-not $NoGpuLog){
        $smi=(Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
        if($smi){
            $gpuCsv=Join-Path $OutputDirectory 'gpu.csv'
            $gpuLog=Start-Process -FilePath $smi -ArgumentList @('--query-gpu=timestamp,utilization.gpu,clocks.gr,power.draw','--format=csv,nounits','-lms','1000','-f',"`"$gpuCsv`"") -WindowStyle Hidden -PassThru
            $receipt.gpu_log=[ordered]@{pid=$gpuLog.Id;tool=$smi;csv=$gpuCsv;clock='local wall time'}
        }else{$receipt.notes+='nvidia-smi not found; no GPU utilization log'}
    }
    Save-Receipt
    $sampler=Find-RepoFile 'build\mod\native_thread_sampler.exe'
    if($SampleHz -gt 0 -and -not $sampler){$receipt.notes+='native_thread_sampler.exe not built; no thread stacks'}
    $trace=Join-Path $OutputDirectory 'cpu_trace.csv'
    $stop=$companionStart.AddSeconds($MaxSeconds)
    while((Get-Date) -lt $stop){
        Start-Sleep -Seconds 1
        $alive=Get-Process -Id $game.Id -ErrorAction SilentlyContinue
        if(-not $alive -or $alive.HasExited){break}
        if($SampleHz -gt 0 -and $sampler -and -not $samplers){
            $threads=Read-TraceThreads $trace
            if($threads -and $threads.pairs -gt 0 -and $threads.render -and $threads.simulation){
                $receipt.threads=$threads
                $count=[Math]::Min(20000,$SampleHz*$SampleSeconds);$interval=[Math]::Max(1,[int](1000/$SampleHz))
                foreach($role in @('render','simulation')){
                    $out=Join-Path $OutputDirectory "sampler_$role.csv"
                    if(Test-Path -LiteralPath $out){continue}
                    $p=Start-Process -FilePath $sampler -ArgumentList @($game.Id,$threads[$role],$count,$interval,"`"$out`"") -WindowStyle Hidden -PassThru `
                        -RedirectStandardError (Join-Path $OutputDirectory "sampler_$role.stderr.txt")
                    $null=$p.Handle;$samplers+=$p
                    $receipt.samplers+=[ordered]@{role=$role;tid=$threads[$role];pid=$p.Id;count=$count;interval_ms=$interval;csv=$out;started_utc=(Get-Date).ToUniversalTime().ToString('o');exit_code=$null}
                }
                Save-Receipt
            }
        }
    }
}finally{
    # Unelevated, --terminate_on_proc_exit does not fire (PresentMon warns so). Stop the session
    # the documented way, so PresentMon finishes its CSV and exits; kill and logman stop only as a fallback.
    if($presentMon){
        $session=$receipt.presentmon.session
        if(-not $presentMon.WaitForExit(3000)){$null=Invoke-Native $tool @('--terminate_existing_session','--session_name',$session)}
        if(-not $presentMon.WaitForExit(15000)){Stop-Process -Id $presentMon.Id -Force -ErrorAction SilentlyContinue;$receipt.notes+='PresentMon killed by the companion'}
        $null=Invoke-Native 'logman.exe' @('stop',$session,'-ets')
        try{$receipt.presentmon.exit_code=$presentMon.ExitCode}catch{}
    }
    # nvidia-smi -lms never exits on its own; stop it by PID (HasExited can throw without a cached handle;
    # on 22 Oct two loggers were left running).
    if($gpuLog){try{Stop-Process -Id $gpuLog.Id -Force -ErrorAction Stop}catch{}}
    # A sampler writes its CSV only after its last sample; one still sampling a dead game exits on its own errors.
    for($i=0;$i -lt $samplers.Count;$i++){
        $p=$samplers[$i]
        if(-not $p.WaitForExit(120000)){Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue;$receipt.notes+="sampler $($p.Id) stopped by the companion"}
        try{$receipt.samplers[$i].exit_code=$p.ExitCode}catch{}
    }
    $receipt.stopped_utc=(Get-Date).ToUniversalTime().ToString('o')
    Save-Receipt
}
