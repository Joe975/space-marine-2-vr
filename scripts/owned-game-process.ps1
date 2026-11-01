# Process ownership for the native no-EAC test launch. Never rediscover a game by
# name/PID after starting it: the retained Process object identifies this launch.
function Start-Sm2OwnedGame {
    param([Parameter(Mandatory)][string]$Executable,[string[]]$ArgumentList=@(),
        [string]$CaptureTool='', [string]$CapturePrefix='', [ref]$CaptureHelper)
    if (-not [IO.Path]::IsPathRooted($Executable)) { throw 'Game executable must be absolute.' }
    $steamEnvironment=@{}
    $launchNames=@('SteamAppId','SteamGameId');if($CaptureTool){$launchNames+='SM2VR_RENDERDOC_PROXY_BRIDGE'}
    foreach ($name in $launchNames) { $steamEnvironment[$name]=@{Present=(Test-Path -LiteralPath ('Env:'+$name));Value=[Environment]::GetEnvironmentVariable($name,'Process')} }
    try {
        $env:SteamAppId='2183900'; $env:SteamGameId='2183900'
        if ($CaptureTool) {
            $env:SM2VR_RENDERDOC_PROXY_BRIDGE='1'
            if ($null -eq $CaptureHelper) { throw 'Capture launch requires a retained helper lease.' }
            return Start-Sm2RenderDocGame -Executable $Executable -ArgumentList $ArgumentList -CaptureTool $CaptureTool -CapturePrefix $CapturePrefix -CaptureHelper $CaptureHelper
        }
        $startOptions=@{FilePath=$Executable;WorkingDirectory=(Split-Path -Parent $Executable);PassThru=$true;ErrorAction='Stop'}
        if ($ArgumentList.Count) { $startOptions.ArgumentList=$ArgumentList }
        Start-Process @startOptions
    } finally {
        foreach ($name in $steamEnvironment.Keys) {
            if ($steamEnvironment[$name].Present) { [Environment]::SetEnvironmentVariable($name,$steamEnvironment[$name].Value,'Process') }
            else { Remove-Item -LiteralPath ('Env:'+$name) -ErrorAction SilentlyContinue }
        }
    }
}

function Get-Sm2OwnedGameExitReceipt {
    param($Process)
    if ($null -eq $Process) { return $null }
    try { $Process.Refresh(); if (-not $Process.HasExited) { return $null } } catch { return $null }
    $receipt=@{source='retained_game_handle';available=$false;status='identity_unavailable';pid=$null;created_utc=$null;exit_code=$null}
    try { $receipt.pid=$Process.Id;$receipt.created_utc=$Process.StartTime.ToUniversalTime().ToString('o') } catch { return $receipt }
    try { $code=$Process.ExitCode;if($code -isnot [int]){throw 'Exit code is unavailable.'};$receipt.exit_code=$code;$receipt.available=$true;$receipt.status='closed' }
    catch { $receipt.status='exit_code_unavailable' }
    return $receipt
}

function ConvertTo-Sm2CaptureArgument {
    param([string]$Value)
    if ($Value -match '["\x00\r\n]') { throw 'Capture arguments cannot contain quotes or control characters.' }
    # Start-Process joins its argument array. Quote each Windows argv token,
    # doubling terminal backslashes so they cannot escape its closing quote.
    return '"'+($Value -replace '(\\+)$','$1$1')+'"'
}

function Assert-Sm2RetainedIdentity {
    param($Process,[int]$ExpectedId,[DateTime]$CreatedUtc,[string]$Executable)
    $Process.Refresh()
    if ($Process.HasExited -or $Process.Id -ne $ExpectedId -or
        $Process.StartTime.ToUniversalTime().Ticks -ne $CreatedUtc.Ticks -or
        -not [string]::Equals([IO.Path]::GetFullPath($Process.MainModule.FileName),$Executable,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Retained capture process identity is no longer live and exact.'
    }
}

function Start-Sm2RenderDocGame {
    param([string]$Executable,[string[]]$ArgumentList,[string]$CaptureTool,[string]$CapturePrefix,[ref]$CaptureHelper)
    foreach ($path in @($Executable,$CaptureTool)) {
        if (-not [IO.Path]::IsPathRooted($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Capture executables must be absolute existing files.' }
    }
    if (-not [IO.Path]::IsPathRooted($CapturePrefix) -or -not (Test-Path -LiteralPath (Split-Path -Parent $CapturePrefix) -PathType Container)) { throw 'Capture prefix needs an absolute existing parent directory.' }
    $gamePath=[IO.Path]::GetFullPath($Executable);$toolPath=[IO.Path]::GetFullPath($CaptureTool)
    $prefix=[IO.Path]::GetFullPath($CapturePrefix)
    $gameName=[IO.Path]::GetFileNameWithoutExtension($gamePath)
    # Repeat the pre-deployment guard at the actual capture-launch boundary.
    if (@(Get-Process -Name $gameName -ErrorAction SilentlyContinue | Where-Object { try {-not $_.HasExited} catch {$true} }).Count) { throw 'The game is already running.' }
    $stdout=$prefix+'.stdout.log';$stderr=$prefix+'.stderr.log'
    # Reserve new files before handing them to Start-Process's redirects. A
    # previous run's diagnostics must never be truncated.
    foreach($path in @($stdout,$stderr)) {
        $reserved=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read);$reserved.Dispose()
    }
    $tokens=@('capture','-w','-d',(Split-Path -Parent $gamePath),'-c',$prefix,$gamePath)+$ArgumentList
    $arguments=@($tokens | ForEach-Object { ConvertTo-Sm2CaptureArgument $_ })
    $helper=Start-Process -FilePath $toolPath -WorkingDirectory (Split-Path -Parent $gamePath) -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ErrorAction Stop
    $CaptureHelper.Value=[pscustomobject]@{Process=$helper;Id=$helper.Id;CreatedUtc=$null;Executable=$toolPath;CapturePrefix=$prefix;Stdout=$stdout;Stderr=$stderr;ExitCode=$null}
    # Open the handle now. All later checks/waits/kills use this object, never
    # a rediscovered helper with the same numeric ID.
    [void]$helper.Handle
    $helperCreated=$helper.StartTime.ToUniversalTime()
    $CaptureHelper.Value.CreatedUtc=$helperCreated
    $deadline=(Get-Date).AddSeconds(15)
    $unavailablePath=$null
    do {
        Assert-Sm2RetainedIdentity $helper $CaptureHelper.Value.Id $helperCreated $toolPath
        $candidates=@(Get-CimInstance Win32_Process -OperationTimeoutSec 3 | Where-Object { $_.Name -eq [IO.Path]::GetFileName($gamePath) })
        if ((Get-Date) -ge $deadline) { throw "Capture helper did not produce a uniquely owned game child within 15 seconds. CIM query completed after the discovery deadline. $unavailablePath" }
        if ($candidates.Count -gt 1) { throw 'Capture game child is ambiguous; no game process was adopted.' }
        if ($candidates.Count -eq 1) {
            $candidate=$candidates[0]
            if ($candidate.ParentProcessId -ne $helper.Id -or
                ($candidate.ExecutablePath -and -not [string]::Equals([IO.Path]::GetFullPath($candidate.ExecutablePath),$gamePath,[StringComparison]::OrdinalIgnoreCase)) -or
                $candidate.CreationDate -isnot [DateTime] -or $candidate.CreationDate.ToUniversalTime() -lt $helperCreated) {
                throw "Capture game candidate has an unqualified parent, path or creation time: pid=$($candidate.ProcessId) parent=$($candidate.ParentProcessId) expected_parent=$($helper.Id) path='$($candidate.ExecutablePath)' expected_path='$gamePath' created='$($candidate.CreationDate)' helper_created='$($helperCreated.ToString('o'))'."
            }
            if (-not $candidate.ExecutablePath) {
                # CIM can briefly expose a new child before its path is readable.
                # This grants no ownership: wait for a fresh complete candidate.
                $unavailablePath="pid=$($candidate.ProcessId) executable path remained unavailable"
                if ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
                continue
            }
            $game=$null
            try {
                $game=Get-Process -Id $candidate.ProcessId -ErrorAction Stop
                [void]$game.Handle
                $created=$game.StartTime.ToUniversalTime()
                # CIM's DMTF timestamp truncates sub-microsecond FILETIME ticks.
                # The retained handle's exact timestamp is used from here on.
                $creationTicks=$created.Ticks-$candidate.CreationDate.ToUniversalTime().Ticks
                if ($created -lt $helperCreated -or $creationTicks -lt 0 -or $creationTicks -gt 9) { throw "Capture child creation changed before its handle was retained: pid=$($game.Id) retained_utc=$($created.ToString('o')) candidate_utc=$($candidate.CreationDate.ToUniversalTime().ToString('o')) creation_delta_ticks=$creationTicks helper_utc=$($helperCreated.ToString('o'))." }
                Assert-Sm2RetainedIdentity $game $candidate.ProcessId $created $gamePath
                Assert-Sm2RetainedIdentity $helper $CaptureHelper.Value.Id $helperCreated $toolPath
                # Verify the direct-parent provenance again while both exact
                # process handles are retained and the helper is still alive.
                $confirm=@(Get-CimInstance Win32_Process -OperationTimeoutSec 3 | Where-Object { $_.ProcessId -eq $game.Id })
                if ($confirm.Count -ne 1 -or $confirm[0].ParentProcessId -ne $helper.Id -or
                    $confirm[0].CreationDate -ne $candidate.CreationDate -or
                    -not [string]::Equals([IO.Path]::GetFullPath($confirm[0].ExecutablePath),$gamePath,[StringComparison]::OrdinalIgnoreCase)) { throw 'Capture child provenance changed during retention.' }
                Assert-Sm2RetainedIdentity $game $candidate.ProcessId $created $gamePath
                if ((Get-Date) -ge $deadline) { throw 'Capture child retention exceeded the 15-second discovery deadline.' }
                return $game
            } catch { if ($null -ne $game) { $game.Dispose() };throw }
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw "Capture helper did not produce a uniquely owned game child within 15 seconds. $unavailablePath"
}

function Stop-Sm2CaptureHelper {
    param([Parameter(Mandatory)]$Lease)
    $helper=$Lease.Process
    $helper.Refresh()
    if (-not $helper.HasExited) {
        Assert-Sm2RetainedIdentity $helper $Lease.Id $Lease.CreatedUtc $Lease.Executable
        if (-not $helper.WaitForExit(5000)) {
            Assert-Sm2RetainedIdentity $helper $Lease.Id $Lease.CreatedUtc $Lease.Executable
            try { $helper.Kill() } catch { if (-not $helper.HasExited) { throw } }
            [void]$helper.WaitForExit(5000)
        }
    }
    if (-not $helper.HasExited) { return $false }
    try { $Lease.ExitCode=$helper.ExitCode } catch { $Lease.ExitCode=$null }
    $helper.Dispose()
    return $true
}

function Get-Sm2OwnedGame {
    param($Process)
    if ($null -eq $Process) { return $null }
    # HasExited queries the retained process handle; it never adopts a reused ID.
    $Process.Refresh()
    if ($Process.HasExited) { return $null }
    return $Process
}

function Stop-Sm2OwnedGame {
    param([Parameter(Mandatory)]$Process,[switch]$NoUiAutomation)
    $owned=Get-Sm2OwnedGame $Process
    if ($null -eq $owned) { return $true }
    if (-not $NoUiAutomation) { [void]$owned.CloseMainWindow() }
    if ($NoUiAutomation -or -not $owned.WaitForExit(20000)) {
        try { $owned.Kill() } catch { if (-not $owned.HasExited) { throw } }
        [void]$owned.WaitForExit(30000)
    }
    return $owned.HasExited
}
