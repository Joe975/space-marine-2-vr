# What the window shows: five numbered steps, each with a status, a detail line, a fix and the
# button it offers. Pure: built from preflight checks and install state, so tests cover the UI's
# decisions without a window.

# SteamVR keeps a player's old binding when the action manifest changes (README Quick start), so
# the player is walked through choosing the default binding once per manifest version.
function Get-Sm2BindingsCurrent {
    param($InstallState, [Parameter(Mandatory)]$Package)
    if (-not $InstallState) { return $false }
    $manifest = Join-Path $Package.Config 'vr_actions.json'
    return $InstallState.bindings_confirmed_sha256 -eq (Get-Sm2Sha256 $manifest)
}

function Confirm-Sm2Bindings {
    param([Parameter(Mandatory)]$Package, [string]$StateRoot = (Get-Sm2StateRoot))
    $state = Read-Sm2InstallState $StateRoot
    if (-not $state) { throw 'Install first.' }
    $state.bindings_confirmed_sha256 = Get-Sm2Sha256 (Join-Path $Package.Config 'vr_actions.json')
    Write-Sm2InstallState $StateRoot $state
}

$Sm2BindingSteps = 'Start SteamVR. In SteamVR open Settings > Controllers > Manage Controller Bindings (or Show old Binding UI), pick Warhammer 40,000: Space Marine 2, and choose the default binding "Index locomotion, interaction and combat" (not a custom one). Then press Done here.'

# Action2: a second button (step 5 offers both play views).
function New-Sm2Step($Number, $Title, $Status, $Detail, $Fix = '', $Action = $null, $Action2 = $null) {
    [pscustomobject]@{ Number = $Number; Title = $Title; Status = $Status; Detail = $Detail; Fix = $Fix; Action = $Action; Action2 = $Action2 }
}

$Sm2PlayViews = 'Motion controls: first person with the Index controllers. Chase camera: the game''s own third-person camera, played with a gamepad.'

# -InstallChecks / -PlayChecks: Test-Sm2Preflight -For install / play. -InstallProblems: from
# Get-Sm2InstallProblems. -Playing: a session is running.
function Get-Sm2StepView {
    param([Parameter(Mandatory)]$InstallChecks, $PlayChecks, [string[]]$InstallProblems = @(), [bool]$BindingsCurrent = $false, [bool]$Playing = $false)
    $byId = @{}; foreach ($c in @($InstallChecks) + @($PlayChecks)) { if ($c -and -not $c.Ok -and -not $byId.ContainsKey($c.Id)) { $byId[$c.Id] = $c } }
    $game = @($InstallChecks | Where-Object { $_.Id -eq 'game' })[0]
    $steps = New-Object System.Collections.Generic.List[object]

    if (-not $game.Ok) {
        $steps.Add((New-Sm2Step 1 'Game found' 'fail' $game.Message $game.Fix 'change'))
        $steps.Add((New-Sm2Step 2 'Steam and SteamVR ready' 'todo' 'Finish step 1 first.'))
        $steps.Add((New-Sm2Step 3 'Game version supported' 'todo' 'Finish step 1 first.'))
        $steps.Add((New-Sm2Step 4 'Install' 'todo' 'Finish steps 1-3 first.'))
        $steps.Add((New-Sm2Step 5 'Play in VR' 'todo' 'Finish steps 1-4 first.'))
        return $steps
    }
    $steps.Add((New-Sm2Step 1 'Game found' 'ok' $game.Message '' 'change'))

    $vrIssues = @('elevation', 'steam', 'steamvr', 'openxr' | Where-Object { $byId.ContainsKey($_) } | ForEach-Object { $byId[$_] })
    if ($vrIssues.Count) {
        $steps.Add((New-Sm2Step 2 'Steam and SteamVR ready' 'fail' (($vrIssues | ForEach-Object { $_.Message }) -join ' ') (($vrIssues | ForEach-Object { $_.Fix }) -join ' ') 'recheck'))
    } else {
        $steps.Add((New-Sm2Step 2 'Steam and SteamVR ready' 'ok' 'Steam is running and SteamVR is the OpenXR runtime.'))
    }

    if ($byId.ContainsKey('version')) {
        $v = $byId['version']
        $steps.Add((New-Sm2Step 3 'Game version supported' 'fail' $v.Message $v.Fix 'updates'))
    } else {
        $steps.Add((New-Sm2Step 3 'Game version supported' 'ok' (@($InstallChecks | Where-Object { $_.Id -eq 'version' })[0].Message)))
    }

    $installBlockers = @($InstallChecks | Where-Object { -not $_.Ok -and $_.Blocking })
    $folder = @('other_mods', 'writable' | Where-Object { $byId.ContainsKey($_) } | ForEach-Object { $byId[$_] })
    if ($Playing) {
        $steps.Add((New-Sm2Step 4 'Install' 'ok' 'Installed.'))
    } elseif ($installBlockers.Count) {
        $action = $(if ($byId.ContainsKey('writable')) { 'fix_permissions' } elseif ($folder.Count) { 'recheck' } else { $null })
        $detail = $(if ($folder.Count) { ($folder | ForEach-Object { $_.Message }) -join ' ' } else { 'Finish the steps above first.' })
        $fix = ($folder | ForEach-Object { $_.Fix }) -join ' '
        $steps.Add((New-Sm2Step 4 'Install' $(if ($folder.Count) { 'fail' } else { 'todo' }) $detail $fix $action))
    } elseif (@($InstallProblems).Count) {
        $detail = $(if (@($InstallProblems) -contains 'not installed') { 'Builds the VR data pack from your game files and backs up your saves.' } else { 'Needs a reinstall: ' + (@($InstallProblems) -join '; ') + '.' })
        $steps.Add((New-Sm2Step 4 'Install' 'todo' $detail '' 'install'))
    } else {
        $steps.Add((New-Sm2Step 4 'Install' 'ok' 'Installed. Your saves were backed up.' '' 'reinstall'))
    }

    if ($Playing) {
        $steps.Add((New-Sm2Step 5 'Play in VR' 'running' 'The game is running. Closing it removes the mod files.'))
    } elseif ($steps[3].Status -ne 'ok') {
        $steps.Add((New-Sm2Step 5 'Play in VR' 'todo' 'Finish steps 1-4 first.'))
    } else {
        $playBlockers = @($PlayChecks | Where-Object { -not $_.Ok -and $_.Blocking })
        # The chase camera uses a gamepad, so it needs neither the SteamVR controller binding nor a pad
        # check to start; a missing pad is only mentioned.
        $pad = $(if ($byId.ContainsKey('gamepad')) { ' ' + $byId['gamepad'].Message + ' ' + $byId['gamepad'].Fix } else { '' })
        if ($playBlockers.Count) {
            $steps.Add((New-Sm2Step 5 'Play in VR' 'fail' (($playBlockers | ForEach-Object { $_.Message }) -join ' ') (($playBlockers | ForEach-Object { $_.Fix }) -join ' ') 'recheck'))
        } elseif (-not $BindingsCurrent) {
            $steps.Add((New-Sm2Step 5 'Play in VR' 'warn' ('Motion controls need a one-time SteamVR setup: choose the mod''s controller binding. The chase camera (gamepad) can be played now.' + $pad) $Sm2BindingSteps 'bindings_done' 'play_chase'))
        } else {
            $leftover = $byId.ContainsKey('leftovers')
            $detail = $(if ($leftover) { 'The last session did not end cleanly; Play removes its leftover files first.' } else { 'Starts the game in VR. The first start takes 3-4 minutes.' })
            $steps.Add((New-Sm2Step 5 'Play in VR' 'ready' ($detail + ' ' + $Sm2PlayViews + $pad) '' 'play' 'play_chase'))
        }
    }
    return $steps
}
