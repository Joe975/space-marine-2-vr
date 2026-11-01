# Keyboard input into the game window, for run-modded.ps1 and drive-game.ps1. Dot-source this file.
# A key is only sent while the game window is the foreground window, so it can never land in another
# application. Scan codes, not virtual keys: the game reads raw keyboard input.

if (-not ('Sm2Keys' -as [type])) { Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Sm2Keys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public static uint ForegroundPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
    // Scan-code press and release (games reading raw or DirectInput keyboards ignore virtual-key-only input).
    // Arrow keys are extended keys: without the flag their scan codes are the numeric keypad's.
    public static bool Press(ushort scan, uint gamePid, bool extended = false) {
        if (ForegroundPid() != gamePid) return false;
        var down = new INPUT { type = 1 }; down.ki.scan = scan; down.ki.flags = 0x0008u | (extended ? 0x0001u : 0u);
        var up = down; up.ki.flags = down.ki.flags | 0x0002;
        SendInput(1, new[] { down }, 40); System.Threading.Thread.Sleep(60); SendInput(1, new[] { up }, 40);
        return true;
    }
    // Hold one key down for ms, then release. Released early (false) if the game loses the foreground,
    // so a held key never lands in another application.
    public static bool Hold(ushort scan, uint gamePid, bool extended, int ms) {
        if (ForegroundPid() != gamePid) return false;
        var down = new INPUT { type = 1 }; down.ki.scan = scan; down.ki.flags = 0x0008u | (extended ? 0x0001u : 0u);
        var up = down; up.ki.flags = down.ki.flags | 0x0002;
        SendInput(1, new[] { down }, 40);
        var held = true;
        for (var waited = 0; waited < ms; waited += 50) {
            System.Threading.Thread.Sleep(50);
            if (ForegroundPid() != gamePid) { held = false; break; }
        }
        SendInput(1, new[] { up }, 40);
        return held;
    }
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    // Windows refuses SetForegroundWindow to a background process unless it sent the last input
    // event; an Alt tap (no menu opens on a bare press/release) satisfies that rule.
    public static bool Activate(IntPtr window) {
        if (window == IntPtr.Zero) return false;
        var alt = new INPUT { type = 1 }; alt.ki.vk = 0x12; var altUp = alt; altUp.ki.flags = 0x0002;
        SendInput(1, new[] { alt }, 40); SendInput(1, new[] { altUp }, 40);
        return SetForegroundWindow(window);
    }
}
'@
}

# F8 cycles the camera mode and F9 recenters (docs/THIRD_PERSON.md); F11 toggles the mod overlay; Q and E change Settings tabs.
$Sm2ScanCodes = @{ ENTER = 0x1C; ESC = 0x01; SPACE = 0x39; F8 = 0x42; F9 = 0x43; F11 = 0x57; Q = 0x10; E = 0x12; W = 0x11; A = 0x1E; S = 0x1F; D = 0x20; UP = 0x48; DOWN = 0x50; LEFT = 0x4B; RIGHT = 0x4D }

# 'W:6' -> key W held 6 s (drive-game.ps1's hold step). Known key, 0 < seconds <= 30.
function ConvertFrom-Sm2HoldStep {
    param([Parameter(Mandatory)][string]$Text)
    $key, $seconds = $Text -split ':', 2
    $key = $key.Trim().ToUpper()
    if (-not $Sm2ScanCodes.ContainsKey($key)) { throw "Unknown key '$key'. Known: $($Sm2ScanCodes.Keys -join ', ')" }
    $value = 0.0
    if (-not [double]::TryParse([string]$seconds, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$value) -or $value -le 0 -or $value -gt 30) {
        throw "hold:$Text needs seconds in (0, 30], e.g. hold:W:6"
    }
    [pscustomobject]@{ Key = $key; Seconds = $value }
}

# Hold one key in the game. False when the window was not in front or lost it (the key is released).
function Send-Sm2KeyHold {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][int]$GamePid, [Parameter(Mandatory)][double]$Seconds)
    $key = $Name.ToUpper()
    if (-not $Sm2ScanCodes.ContainsKey($key)) { throw "Unknown key '$Name'. Known: $($Sm2ScanCodes.Keys -join ', ')" }
    [Sm2Keys]::Hold([uint16]$Sm2ScanCodes[$key], [uint32]$GamePid, ($key -in 'UP', 'DOWN', 'LEFT', 'RIGHT'), [int]($Seconds * 1000))
}

# One key into the game. False when the game window is not in front (nothing is sent then).
function Send-Sm2Key {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][int]$GamePid)
    $key = $Name.ToUpper()
    if (-not $Sm2ScanCodes.ContainsKey($key)) { throw "Unknown key '$Name'. Known: $($Sm2ScanCodes.Keys -join ', ')" }
    [Sm2Keys]::Press([uint16]$Sm2ScanCodes[$key], [uint32]$GamePid, ($key -in 'UP', 'DOWN', 'LEFT', 'RIGHT'))
}

# Brings the game window to the front if it isn't. A game started from a background process may
# never get the foreground on its own (Windows' foreground lock).
function Set-Sm2GameForeground {
    param([Parameter(Mandatory)][int]$GamePid)
    if ([Sm2Keys]::ForegroundPid() -eq [uint32]$GamePid) { return $true }
    $p = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
    if (-not $p) { return $false }
    $ok = [Sm2Keys]::Activate($p.MainWindowHandle); Start-Sleep -Milliseconds 500
    $ok -and [Sm2Keys]::ForegroundPid() -eq [uint32]$GamePid
}
