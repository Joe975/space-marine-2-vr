# Saves a PNG of a process's main window (works when occluded; uses PrintWindow with full-content flag),
# or of the primary screen when -ProcessId is omitted.
# Usage: screenshot.ps1 -Out <path.png> [-ProcessId <pid>] [-MaxWidth 1280]
param([Parameter(Mandatory)][string]$Out, [int]$ProcessId = 0, [int]$MaxWidth = 1280)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Sm2Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[void][Sm2Win]::SetProcessDPIAware()
if ($ProcessId) {
    $h = (Get-Process -Id $ProcessId).MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "process $ProcessId has no main window" }
    $r = New-Object Sm2Win+RECT
    [void][Sm2Win]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][Sm2Win]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $where = "window at ($($r.L),$($r.T)) ${w}x${ht}"
} else {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $where = "primary screen $($b.Width)x$($b.Height)"
}
$scale = [Math]::Min(1.0, $MaxWidth / $bmp.Width)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($bmp.Width * $scale)), ([int]($bmp.Height * $scale))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"$Out $($small.Width)x$($small.Height) from $where"
$small.Dispose()
