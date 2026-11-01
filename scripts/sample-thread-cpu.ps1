param(
    [int]$GameProcessId,
    [ValidateRange(1,120)][int]$Seconds=30,
    [Parameter(Mandatory)][string]$OutputCsv,
    [string]$BeforeCsv,
    [string]$AfterCsv,
    [double]$ElapsedSeconds=0
)
$ErrorActionPreference='Stop'
if(-not [IO.Path]::IsPathRooted($OutputCsv)){throw 'Absolute output path required'}
if(Test-Path -LiteralPath $OutputCsv){throw 'Output already exists'}
if($BeforeCsv -or $AfterCsv){
    if(-not $BeforeCsv -or -not $AfterCsv -or $ElapsedSeconds -le 0){throw 'Both fixtures and elapsed seconds required'}
    $before=Import-Csv -LiteralPath $BeforeCsv
    $after=Import-Csv -LiteralPath $AfterCsv
}else{
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Sm2ThreadNames {
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr thread, out IntPtr name);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    public static string Read(uint id) {
        IntPtr h=OpenThread(0x0800,false,id); if(h==IntPtr.Zero)return "";
        try { IntPtr p; if(GetThreadDescription(h,out p)!=0)return "";
            try{return Marshal.PtrToStringUni(p)??"";}finally{LocalFree(p);}
        }finally{CloseHandle(h);}
    }
}
'@
    function Snapshot {
        $process=Get-Process -Id $GameProcessId
        foreach($thread in $process.Threads){
            try{[pscustomobject]@{tid=$thread.Id;description=[Sm2ThreadNames]::Read($thread.Id);total_ms=$thread.TotalProcessorTime.TotalMilliseconds}}catch{}
        }
    }
    $before=@(Snapshot)
    $timer=[Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $Seconds
    $after=@(Snapshot)
    $ElapsedSeconds=$timer.Elapsed.TotalSeconds
}
$initial=@{}
foreach($row in $before){$initial[[string]$row.tid]=[double]$row.total_ms}
$result=@(foreach($row in $after){
    if(-not $initial.ContainsKey([string]$row.tid)){continue}
    $delta=[double]$row.total_ms-$initial[[string]$row.tid]
    if($delta -lt 0){continue}
    [pscustomobject]@{tid=[int]$row.tid;description=$row.description;cpu_ms=$delta;percent_of_one_core=100*$delta/($ElapsedSeconds*1000)}
}) | Sort-Object @{Expression='cpu_ms';Descending=$true},tid
$result | Export-Csv -LiteralPath $OutputCsv -NoTypeInformation
$result | Select-Object -First 8 | Format-Table -AutoSize
