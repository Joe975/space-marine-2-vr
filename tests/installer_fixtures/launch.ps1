# Drives Start-Sm2Play / Repair-Sm2AfterCrash / Uninstall-Sm2 on a fixture game whose exe is a
# renamed cmd.exe. -Pretend writes an install.json as Install-Sm2 would (no builders needed).
param([string]$Mode, [string]$PackageRoot, [string]$GameRoot, [string]$StateRoot, [string]$LogRoot, [string]$GameCommand = '', [switch]$Pretend, [string]$View = 'motion')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
foreach ($part in 'sm2-game', 'sm2-profile', 'sm2-install', 'sm2-launch') { . (Join-Path $PSScriptRoot "..\..\installer\app\$part.ps1") }
$package = Get-Sm2Package $PackageRoot
$game = Find-Sm2Game -Override $GameRoot
if ($Pretend) {
    New-Item -ItemType Directory -Force -Path (Join-Path $StateRoot 'pack') | Out-Null
    Install-Sm2Config -Package $package -StateRoot $StateRoot | Out-Null
    $pak = Join-Path $StateRoot 'pack\SM2VR_FirstPerson_Test.pak'
    [IO.File]::WriteAllText($pak, 'pack')
    $layout = [regex]::Match([IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\..\scripts\build_vr_controls.py')), '(?m)^INPUT_LAYOUT = (\d+)').Groups[1].Value
    [IO.File]::WriteAllText([IO.Path]::ChangeExtension($pak, '.json'), "{`"input_layout`": $layout}")
    New-Item -ItemType Directory -Force -Path (Join-Path $StateRoot 'pack\chase') | Out-Null
    $chase = Join-Path $StateRoot 'pack\chase\SM2VR_FirstPerson_Test.pak'
    [IO.File]::WriteAllText($chase, 'chase')
    Write-Sm2InstallState $StateRoot ([ordered]@{ schema = 1; mod_version = $package.Version.mod_version; game_root = $game.Root
        exe_timestamp = (Get-Sm2PeTimestamp $game.Exe)
        pack = @{ path = $pak; sha256 = (Get-Sm2Sha256 $pak); chase = @{ path = $chase; sha256 = (Get-Sm2Sha256 $chase) } }
        grenade_layout_verified = $true; game_files = @() })
}
switch ($Mode) {
    'play' {
        $phases = New-Object System.Collections.Generic.List[string]
        $r = Start-Sm2Play -Package $package -Game $game -StateRoot $StateRoot -LogRoot $LogRoot -GameArguments @($GameCommand) -PollMilliseconds 100 -View $View -OnPhase { param($p) $phases.Add($p.Id) }
        @{ result = $r; seen = @($phases) } | ConvertTo-Json -Depth 4
    }
    'repair' { $r = Repair-Sm2AfterCrash -Game $game -StateRoot $StateRoot -LogRoot $LogRoot; if ($null -eq $r) { 'null' } else { $r | ConvertTo-Json } }
    'uninstall' { ConvertTo-Json -InputObject @(Uninstall-Sm2 -Game $game -StateRoot $StateRoot -LogRoot $LogRoot) }
    'phases' { ConvertTo-Json -InputObject @(Get-Sm2StartupPhases ([IO.File]::ReadAllLines($GameCommand)) | ForEach-Object { $_.Id }) }
    # -GameCommand is a folder of batch files 0.txt, 1.txt, ...: each is appended to the log, then
    # the play loop's poll runs once. One record per poll.
    'watch' {
        $log = Join-Path $LogRoot 'sm2vr.log'; $offset = 0; $watch = New-Sm2PhaseWatch
        $polls = foreach ($i in 0..((@(Get-ChildItem $GameCommand -Filter '*.txt')).Count - 1)) {
            [IO.File]::AppendAllText($log, [IO.File]::ReadAllText((Join-Path $GameCommand "$i.txt")))
            $before = $watch.Examined
            $ids = @(Update-Sm2PhaseWatch -Watch $watch -Path $log -Offset ([ref]$offset) | ForEach-Object { $_.Id })
            [pscustomobject]@{ phases = $ids; examined = $watch.Examined - $before; offset = $offset; done = $watch.Done }
        }
        ConvertTo-Json -InputObject @($polls) -Depth 3
    }
}
