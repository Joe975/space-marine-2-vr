# SM2VR window: five numbered steps (Get-Sm2StepView decides what they say) and a log.
# Long jobs (install, play, update) run in a background runspace so the window stays responsive.
# -SelfTest builds and fills the window without showing it, prints a summary and exits (tests).
# -Screenshot <png> (with -SelfTest) also renders the window off-screen to that file.
param([switch]$SelfTest, [string]$Screenshot = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
# The console is hidden (SM2VR.cmd), so a startup error must show as a dialog.
trap {
    if ($SelfTest) { throw $_ }
    [Windows.Forms.MessageBox]::Show("SM2VR could not start:`n`n$($_.Exception.Message)`n`n$($_.InvocationInfo.PositionMessage)", 'SM2VR') | Out-Null
    break
}

$Sm2AppDir = $PSScriptRoot
$Sm2Parts = 'sm2-game', 'sm2-profile', 'sm2-preflight', 'sm2-install', 'sm2-launch', 'sm2-update', 'sm2-report', 'sm2-view'
foreach ($part in $Sm2Parts) { . (Join-Path $Sm2AppDir "$part.ps1") }

$packageRoot = Split-Path -Parent $Sm2AppDir
$package = Get-Sm2Package $packageRoot
$stateRoot = Get-Sm2StateRoot
New-Item -ItemType Directory -Force -Path $stateRoot | Out-Null
$settingsPath = Join-Path $stateRoot 'settings.json'
$settings = Read-Sm2Json $settingsPath
if (-not $settings) { $settings = [pscustomobject]@{ game_root = $null; channel = 'stable' } }
$ui = @{ Game = $null; Busy = $false; Playing = $false; Job = $null }

function Save-Settings { Write-Sm2Json $settingsPath $settings }

# --- window -----------------------------------------------------------------------------------
$form = New-Object Windows.Forms.Form
$form.Text = "SM2VR $($package.Version.mod_version) - Space Marine 2 in VR"
$form.Size = New-Object Drawing.Size(820, 760)
$form.MinimumSize = New-Object Drawing.Size(700, 600)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Segoe UI', 9.5)

$layout = New-Object Windows.Forms.TableLayoutPanel
$layout.Dock = 'Fill'; $layout.ColumnCount = 1; $layout.RowCount = 3; $layout.Padding = New-Object Windows.Forms.Padding(10)
[void]$layout.RowStyles.Add((New-Object Windows.Forms.RowStyle('AutoSize')))
[void]$layout.RowStyles.Add((New-Object Windows.Forms.RowStyle('Percent', 100)))
[void]$layout.RowStyles.Add((New-Object Windows.Forms.RowStyle('AutoSize')))
$form.Controls.Add($layout)

$stepsPanel = New-Object Windows.Forms.TableLayoutPanel
$stepsPanel.Dock = 'Top'; $stepsPanel.AutoSize = $true; $stepsPanel.ColumnCount = 3
[void]$stepsPanel.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Absolute', 90)))
[void]$stepsPanel.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Percent', 100)))
[void]$stepsPanel.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Absolute', 175)))
$layout.Controls.Add($stepsPanel, 0, 0)

$rows = @()
for ($i = 0; $i -lt 5; $i++) {
    $status = New-Object Windows.Forms.Label
    $status.AutoSize = $false; $status.Size = New-Object Drawing.Size(84, 24); $status.TextAlign = 'MiddleCenter'
    $status.Font = New-Object Drawing.Font('Segoe UI', 9, [Drawing.FontStyle]::Bold); $status.ForeColor = 'White'
    $text = New-Object Windows.Forms.Label
    $text.AutoSize = $true; $text.MaximumSize = New-Object Drawing.Size(520, 0); $text.Margin = New-Object Windows.Forms.Padding(3, 3, 3, 10)
    $buttons = New-Object Windows.Forms.FlowLayoutPanel
    $buttons.FlowDirection = 'TopDown'; $buttons.AutoSize = $true; $buttons.WrapContents = $false; $buttons.Margin = New-Object Windows.Forms.Padding(0)
    $button = New-Object Windows.Forms.Button
    $button.Size = New-Object Drawing.Size(165, 30); $button.Visible = $false
    $button2 = New-Object Windows.Forms.Button
    $button2.Size = New-Object Drawing.Size(165, 30); $button2.Visible = $false
    $buttons.Controls.Add($button); $buttons.Controls.Add($button2)
    $stepsPanel.Controls.Add($status, 0, $i); $stepsPanel.Controls.Add($text, 1, $i); $stepsPanel.Controls.Add($buttons, 2, $i)
    $rows += [pscustomobject]@{ Status = $status; Text = $text; Button = $button; Button2 = $button2 }
}

$log = New-Object Windows.Forms.TextBox
$log.Multiline = $true; $log.ReadOnly = $true; $log.ScrollBars = 'Vertical'; $log.Dock = 'Fill'
$log.Font = New-Object Drawing.Font('Consolas', 9); $log.BackColor = 'White'
$layout.Controls.Add($log, 0, 1)

$bar = New-Object Windows.Forms.FlowLayoutPanel
$bar.Dock = 'Fill'; $bar.AutoSize = $true; $bar.WrapContents = $true
$layout.Controls.Add($bar, 0, 2)
function New-BarButton([string]$Text) { $b = New-Object Windows.Forms.Button; $b.Text = $Text; $b.AutoSize = $true; $b.Height = 30; $bar.Controls.Add($b); return $b }
$controlsButton = New-BarButton 'Controls'
$reportButton = New-BarButton 'Make crash report'
$updatesButton = New-BarButton 'Check for updates'
$logsButton = New-BarButton 'Open logs folder'
$uninstallButton = New-BarButton 'Uninstall'
$channelLabel = New-Object Windows.Forms.Label; $channelLabel.Text = 'Channel:'; $channelLabel.AutoSize = $true; $channelLabel.Margin = New-Object Windows.Forms.Padding(12, 8, 0, 0); $bar.Controls.Add($channelLabel)
$channelBox = New-Object Windows.Forms.ComboBox; $channelBox.DropDownStyle = 'DropDownList'; [void]$channelBox.Items.AddRange(@('Stable', 'Beta')); $channelBox.Width = 90; $bar.Controls.Add($channelBox)
$channelBox.SelectedIndex = $(if ($settings.channel -eq 'beta') { 1 } else { 0 })
# Read at the next Play (Set-Sm2MainHand). Saved here, not in launch.json: an update replaces that.
# One group, so the label never wraps away from its box on the narrow window.
$handGroup = New-Object Windows.Forms.FlowLayoutPanel; $handGroup.AutoSize = $true; $handGroup.WrapContents = $false; $handGroup.Margin = New-Object Windows.Forms.Padding(0); $bar.Controls.Add($handGroup)
$handLabel = New-Object Windows.Forms.Label; $handLabel.Text = 'Main hand:'; $handLabel.AutoSize = $true; $handLabel.Margin = New-Object Windows.Forms.Padding(4, 8, 0, 0); $handGroup.Controls.Add($handLabel)
$handBox = New-Object Windows.Forms.ComboBox; $handBox.DropDownStyle = 'DropDownList'; [void]$handBox.Items.AddRange(@($Sm2MainHands.Values)); $handBox.Width = 170; $handGroup.Controls.Add($handBox)
$handBox.SelectedIndex = [Math]::Max(0, [array]::IndexOf(@($Sm2MainHands.Keys), [string]$settings.main_hand))
$handBox.Add_SelectedIndexChanged({ $settings | Add-Member -Force NoteProperty main_hand @($Sm2MainHands.Keys)[$handBox.SelectedIndex]; Save-Settings })

$colors = @{ ok = 'SeaGreen'; ready = 'SeaGreen'; running = 'SteelBlue'; warn = 'DarkOrange'; fail = 'Firebrick'; todo = 'Gray' }
$words = @{ ok = 'OK'; ready = 'READY'; running = 'RUNNING'; warn = 'ONE STEP'; fail = 'PROBLEM'; todo = 'WAITING' }
$buttonText = @{ change = 'Change folder...'; recheck = 'Check again'; updates = 'Check for updates'; fix_permissions = 'Fix permissions'
    install = 'Install'; reinstall = 'Reinstall'; play = 'Play: motion controls'; play_chase = 'Play: chase camera' }

function Write-Log([string]$Message) {
    $log.AppendText(('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message) + [Environment]::NewLine)
}

# --- state -> steps ---------------------------------------------------------------------------
function Update-View {
    $ui.Game = $null
    try {
        # A folder picked with Change wins; otherwise Steam (or SM2VR_GAME_ROOT).
        if ($settings.game_root) { $ui.Game = Find-Sm2Game -Override $settings.game_root } else { $ui.Game = Find-Sm2Game }
    } catch { Write-Log "Saved game folder is not valid: $($_.Exception.Message)" }
    $install = Read-Sm2InstallState $stateRoot
    $owned = @(); if ($install) { $owned = @(@($install.game_files) | Where-Object { $_ } | ForEach-Object { Split-Path -Leaf $_.path }) }
    $system = Get-Sm2SystemState -Game $ui.Game -Version $package.Version -OwnedNames $owned -StateRoot $stateRoot
    $installChecks = Test-Sm2Preflight -State $system -For install
    $playChecks = Test-Sm2Preflight -State $system -For play
    $problems = @(); if ($ui.Game) { $problems = @(Get-Sm2InstallProblems -InstallState $install -Package $package -Game $ui.Game) }
    $steps = Get-Sm2StepView -InstallChecks $installChecks -PlayChecks $playChecks -InstallProblems $problems -Playing $ui.Playing
    for ($i = 0; $i -lt 5; $i++) {
        $s = $steps[$i]; $r = $rows[$i]
        $r.Status.Text = $words[$s.Status]; $r.Status.BackColor = [Drawing.Color]::FromName($colors[$s.Status])
        $r.Text.Text = "$($s.Number). $($s.Title)`n$($s.Detail)" + $(if ($s.Fix) { "`nFix: $($s.Fix)" } else { '' })
        $r.Button.Visible = [bool]$s.Action
        $r.Button.Tag = $s.Action
        if ($s.Action) { $r.Button.Text = $buttonText[$s.Action] }
        $r.Button.Enabled = -not $ui.Busy
        $r.Button2.Visible = [bool]$s.Action2
        $r.Button2.Tag = $s.Action2
        if ($s.Action2) { $r.Button2.Text = $buttonText[$s.Action2] }
        $r.Button2.Enabled = -not $ui.Busy
    }
    foreach ($b in $reportButton, $updatesButton, $uninstallButton, $channelBox, $handBox) { $b.Enabled = -not $ui.Busy }
    return $steps
}

# --- background jobs --------------------------------------------------------------------------
# $Work is a script text run with $payload and $sync in scope; it reports through $sync.Say.
function Start-Job2([string]$Title, [string]$Work, $Payload, [scriptblock]$OnDone = {}) {
    $sync = [hashtable]::Synchronized(@{ Messages = [Collections.ArrayList]::Synchronized((New-Object Collections.ArrayList)); Done = $false; Error = $null; Result = $null })
    $ps = [powershell]::Create()
    [void]$ps.AddScript({
        param($appDir, $parts, $work, $payload, $sync)
        $ErrorActionPreference = 'Stop'
        try {
            foreach ($part in $parts) { . (Join-Path $appDir "$part.ps1") }
            $say = { param($m) [void]$sync.Messages.Add([string]$m) }
            $sync.Result = & ([scriptblock]::Create($work)) $payload $say
        } catch { $sync.Error = $_.Exception.Message } finally { $sync.Done = $true }
    }).AddArgument($Sm2AppDir).AddArgument($Sm2Parts).AddArgument($Work).AddArgument($Payload).AddArgument($sync)
    $ui.Busy = $true; $ui.Job = @{ Ps = $ps; Handle = $ps.BeginInvoke(); Sync = $sync; Title = $Title; OnDone = $OnDone }
    Write-Log "$Title..."
    Update-View | Out-Null
    $timer.Start()
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 400
$timer.Add_Tick({
    $job = $ui.Job
    if (-not $job) { $timer.Stop(); return }
    while ($job.Sync.Messages.Count) { Write-Log $job.Sync.Messages[0]; $job.Sync.Messages.RemoveAt(0) }
    if (-not $job.Sync.Done) { return }
    $timer.Stop()
    try { $job.Ps.EndInvoke($job.Handle) | Out-Null } catch {}
    $job.Ps.Dispose()
    $ui.Job = $null; $ui.Busy = $false; $ui.Playing = $false
    if ($job.Sync.Error) { Write-Log "$($job.Title) failed: $($job.Sync.Error)" } else { & $job.OnDone $job.Sync.Result }
    Update-View | Out-Null
})

# --- actions ----------------------------------------------------------------------------------
function Invoke-Install {
    Start-Job2 'Installing' {
        param($p, $say)
        $state = Install-Sm2 -Package $p.Package -Game $p.Game -StateRoot $p.StateRoot -Progress $say
        & $say "Installed. Saves backed up to $($state.last_save_backup)."
    }.ToString() @{ Package = $package; Game = $ui.Game; StateRoot = $stateRoot }
}

function Invoke-Play([ValidateSet('motion', 'chase')][string]$View = 'motion') {
    if ((Get-Sm2ProcessState 'vrserver') -eq 'not_running') { Write-Log 'Starting SteamVR.'; Start-Process 'steam://run/250820' }
    $ui.Playing = $true
    $title = $(if ($View -eq 'chase') { 'Starting Space Marine 2 in VR (chase camera, gamepad)' } else { 'Starting Space Marine 2 in VR (motion controls)' })
    Start-Job2 $title {
        param($p, $say)
        $r = Start-Sm2Play -Package $p.Package -Game $p.Game -StateRoot $p.StateRoot -View $p.View -OnPhase { param($phase) & $say $phase.Text }
        & $say "The game closed (exit code $($r.ExitCode)). Mod files removed. Logs: $($r.RunDir)"
        foreach ($w in $r.Warnings) { & $say "Warning: $w" }
    }.ToString() @{ Package = $package; Game = $ui.Game; StateRoot = $stateRoot; View = $View }
}

function Invoke-Change {
    $dialog = New-Object Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Pick the Space Marine 2 folder (the one that holds client_pc).'
    if ($dialog.ShowDialog($form) -ne 'OK') { return }
    try {
        Find-Sm2Game -Override $dialog.SelectedPath | Out-Null
        $settings.game_root = $dialog.SelectedPath; Save-Settings
        Write-Log "Game folder set to $($dialog.SelectedPath)."
    } catch { [Windows.Forms.MessageBox]::Show($form, 'That folder does not hold Space Marine 2 (no client_pc\root\bin\pc game exe).', 'SM2VR') | Out-Null }
}

function Invoke-Updates {
    $settings.channel = $(if ($channelBox.SelectedIndex -eq 1) { 'beta' } else { 'stable' }); Save-Settings
    if (-not $package.Version.manifest_url) { Write-Log 'This build has no update address.'; return }
    Start-Job2 'Checking for updates' {
        param($p, $say)
        $manifest = Get-Sm2Manifest $p.Url
        $timestamp = $null; if ($p.Game) { $timestamp = Get-Sm2PeTimestamp $p.Game.Exe }
        return Get-Sm2UpdatePlan -Manifest $manifest -CurrentVersion $p.Version -Channel $p.Channel -GameExeTimestamp $timestamp
    }.ToString() @{ Url = $package.Version.manifest_url; Version = $package.Version.mod_version; Channel = $settings.channel; Game = $ui.Game } {
        param($plan)
        Write-Log $plan.Message
        if ($plan.Action -ne 'update') { return }
        $answer = [Windows.Forms.MessageBox]::Show($form, "$($plan.Message)`n`nChanges:`n$($plan.Release.changelog)`n`nDownload and install it now?", 'SM2VR update', 'YesNo')
        if ($answer -ne 'Yes') { return }
        Start-Job2 'Updating' {
            param($p, $say)
            Backup-Sm2Saves -StateRoot $p.StateRoot | Out-Null
            & $say "Downloading $($p.Release.version)"
            $zip = Save-Sm2Release -Release $p.Release -Directory (Join-Path $p.StateRoot 'downloads')
            $v = Install-Sm2Update -Zip $zip -Sha256 $p.Release.sha256 -PackageRoot $p.Root
            Remove-Item -LiteralPath $zip -Force
            return $v
        }.ToString() @{ Release = $plan.Release; StateRoot = $stateRoot; Root = $packageRoot } {
            param($v)
            [Windows.Forms.MessageBox]::Show($form, "Updated to $v. SM2VR restarts now; press Install when it asks.", 'SM2VR') | Out-Null
            Start-Process -FilePath (Join-Path $packageRoot 'SM2VR.cmd') -WorkingDirectory $packageRoot
            $form.Close()
        }
    }
}

function Invoke-Report {
    try {
        $zip = New-Sm2CrashReport -Package $package -Game $ui.Game -StateRoot $stateRoot -OutDirectory ([Environment]::GetFolderPath('Desktop'))
        Write-Log "Crash report: $zip. It holds logs, not memory dumps; logs can include your Windows user name."
        Start-Process explorer.exe -ArgumentList "/select,`"$zip`""
    } catch { Write-Log "Crash report failed: $($_.Exception.Message)" }
}

function Invoke-Uninstall {
    if (-not $ui.Game) { Write-Log 'Game not found; nothing to remove from it.'; return }
    $answer = [Windows.Forms.MessageBox]::Show($form, 'Remove the mod? Your save backups and logs stay in ' + $stateRoot + '.', 'SM2VR', 'YesNo')
    if ($answer -ne 'Yes') { return }
    try {
        foreach ($w in (Uninstall-Sm2 -Game $ui.Game -StateRoot $stateRoot)) { Write-Log "Warning: $w" }
        Write-Log "Uninstalled. The game folder has no mod files. You can delete $packageRoot."
    } catch { Write-Log "Uninstall failed: $($_.Exception.Message)" }
}

foreach ($stepButton in @($rows | ForEach-Object { $_.Button; $_.Button2 })) {
    $stepButton.Add_Click({
        param($sender)
        switch ($sender.Tag) {
            'change' { Invoke-Change }
            'recheck' { Write-Log 'Checking again.' }
            'updates' { Invoke-Updates }
            'fix_permissions' { if (Grant-Sm2GameFolderAccess -Game $ui.Game) { Write-Log 'Permissions fixed.' } else { Write-Log 'Permissions were not changed.' } }
            'install' { Invoke-Install }
            'reinstall' { Invoke-Install }
            'play' { Invoke-Play 'motion' }
            'play_chase' { Invoke-Play 'chase' }
        }
        if (-not $ui.Busy) { Update-View | Out-Null }
    })
}
$controlsButton.Add_Click({ $page = Join-Path $packageRoot 'docs\CONTROLS.html'; if (Test-Path -LiteralPath $page) { Start-Process $page } else { Write-Log 'No controls page in this build.' } })
$reportButton.Add_Click({ Invoke-Report })
$updatesButton.Add_Click({ Invoke-Updates })
$logsButton.Add_Click({ New-Item -ItemType Directory -Force -Path (Join-Path $stateRoot 'runs') | Out-Null; Start-Process explorer.exe -ArgumentList "`"$(Join-Path $stateRoot 'runs')`"" })
$uninstallButton.Add_Click({ Invoke-Uninstall })
$form.Add_FormClosing({
    param($sender, $e)
    if ($ui.Playing) {
        [Windows.Forms.MessageBox]::Show($form, 'Close the game first; SM2VR removes the mod files when it exits.', 'SM2VR') | Out-Null
        $e.Cancel = $true
    }
})

# --- start ------------------------------------------------------------------------------------
$steps = Update-View
# Not in a self-test, which only looks: a repair moments before a launch's game process appears would
# take its files out of the game folder.
if (-not $SelfTest -and $ui.Game -and -not ($ui.Game.Bin -and (Get-Sm2BinProcesses $ui.Game.Bin).Count)) {
    try {
        $repair = Repair-Sm2AfterCrash -Game $ui.Game -StateRoot $stateRoot
        if ($repair) {
            Write-Log "The last session ended unexpectedly; its mod files were removed. Logs: $($repair.RunDir). Make crash report packs them up."
            foreach ($w in $repair.Warnings) { Write-Log "Warning: $w" }
            $steps = Update-View
        }
    } catch { Write-Log "Could not clean up the last session: $($_.Exception.Message)" }
}
Write-Log "SM2VR $($package.Version.mod_version) for game version $($package.Version.game_build_date). State: $stateRoot"
if ($SelfTest) {
    if ($Screenshot) {
        $form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(-3000, -3000); $form.ShowInTaskbar = $false
        $form.Show(); [Windows.Forms.Application]::DoEvents()
        $image = New-Object Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($image, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $image.Save($Screenshot, [Drawing.Imaging.ImageFormat]::Png); $image.Dispose()
        $form.Hide()
    }
    @{ steps = @($steps | ForEach-Object { @{ status = $_.Status; action = $_.Action; title = $_.Title } }); log = $log.Text; main_hand = [string]$handBox.SelectedItem } | ConvertTo-Json -Depth 4
    $form.Dispose()
    return
}
[void]$form.ShowDialog()
