# Install = the mod's own folder is ready: config copied, data pack built from the player's game
# files, saves backed up. Nothing is left in the game folder; launch copies files in and out.

$Sm2Utf8 = New-Object Text.UTF8Encoding $false

# SM2VR_PLAYER_STATE moves it (tests, or a second install side by side).
function Get-Sm2StateRoot {
    if ($env:SM2VR_PLAYER_STATE) { return $env:SM2VR_PLAYER_STATE }
    return (Join-Path $env:LOCALAPPDATA 'sm2vr\player')
}

# The unpacked release (see docs/INSTALLER_PLAN.md "Package layout").
function Get-Sm2Package([string]$Root) {
    $version = [IO.File]::ReadAllText((Join-Path $Root 'version.json')) | ConvertFrom-Json
    [pscustomobject]@{
        Root = $Root
        Version = $version
        Dll = Join-Path $Root 'mod\dxgi.dll'
        OpenVrDll = Join-Path $Root 'mod\openvr_api.dll'
        Profile = Join-Path $Root 'mod\launch.json'
        Config = Join-Path $Root 'mod\config'
        Builders = Join-Path $Root 'builders'
        Python = Join-Path $Root 'python\python.exe'
    }
}

function Read-Sm2Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return [IO.File]::ReadAllText($Path) | ConvertFrom-Json
}

function Write-Sm2Json([string]$Path, $Value) {
    $temp = $Path + '.tmp'
    [IO.File]::WriteAllText($temp, ($Value | ConvertTo-Json -Depth 6), $Sm2Utf8)
    Move-Item -LiteralPath $temp -Destination $Path -Force
}

function Read-Sm2InstallState([string]$StateRoot) { Read-Sm2Json (Join-Path $StateRoot 'install.json') }
function Write-Sm2InstallState([string]$StateRoot, $State) { Write-Sm2Json (Join-Path $StateRoot 'install.json') $State }

function Get-Sm2Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# key=value calibration file -> ordered key->value ('#' comments and blank lines skipped).
function Read-Sm2Calibration([string]$Path) {
    $values = [ordered]@{}
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $values }
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#') -or $t -notmatch '^([^=]+)=(.*)$') { continue }
        $values[$Matches[1].Trim()] = $Matches[2].Trim()
    }
    return $values
}

# Three-way merge of one preserved calibration file. The result is the new release's file (its
# comments, keys and order) with the player's value for each key the player changed, meaning it
# differs from the release copy they were given (-Previous). Without -Previous every value in the
# player's file counts as theirs. Keys the release dropped go too: the DLL's readers reject unknown
# keys. Returns @{ Text; Kept = keys that kept the player's value }.
function Merge-Sm2Calibration {
    param([Parameter(Mandatory)][string]$Release, [Parameter(Mandatory)][string]$Player, [string]$Previous)
    $mine = Read-Sm2Calibration $Player
    $before = Read-Sm2Calibration $Previous
    $hasPrevious = $Previous -and (Test-Path -LiteralPath $Previous)
    $kept = @()
    $lines = foreach ($line in [IO.File]::ReadAllLines($Release)) {
        $t = $line.Trim()
        if ($t -and -not $t.StartsWith('#') -and $t -match '^([^=]+)=(.*)$') {
            $key = $Matches[1].Trim()
            if ($mine.Contains($key) -and (-not $hasPrevious -or -not $before.Contains($key) -or $mine[$key] -ne $before[$key])) {
                if ($mine[$key] -ne $Matches[2].Trim()) { $kept += $key }
                "$key=$($mine[$key])"
                continue
            }
        }
        $line
    }
    return @{ Text = (($lines -join "`n") + "`n"); Kept = $kept }
}

# Copies the shipped config. Files named in version.json "preserve" that the player already has
# (their F11 tuning) are merged key by key (Merge-Sm2Calibration); everything else is replaced by
# the release's copy. The shipped copies are kept in config-shipped\ as the next merge's base.
# Returns "file: key, key" for each preserved file where the player's values were kept.
function Install-Sm2Config {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)][string]$StateRoot)
    $target = Join-Path $StateRoot 'config'
    $shipped = Join-Path $StateRoot 'config-shipped'
    New-Item -ItemType Directory -Force -Path $target, $shipped | Out-Null
    $preserve = @($Package.Version.preserve)
    $kept = @()
    foreach ($file in Get-ChildItem -LiteralPath $Package.Config -File) {
        $destination = Join-Path $target $file.Name
        if ((Test-Path -LiteralPath $destination) -and ($preserve -contains $file.Name)) {
            $merge = Merge-Sm2Calibration -Release $file.FullName -Player $destination -Previous (Join-Path $shipped $file.Name)
            [IO.File]::WriteAllText($destination, $merge.Text, $Sm2Utf8)
            if ($merge.Kept.Count) { $kept += "$($file.Name): $($merge.Kept -join ', ')" }
        } else {
            Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
        }
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $shipped $file.Name) -Force
    }
    return $kept
}

# Runs one Python script with the bundled interpreter; throws with its output on failure.
function Invoke-Sm2Python {
    param([string]$Python, [string]$Script, [string[]]$Arguments)
    # Windows PowerShell turns native stderr lines into errors; 'Stop' would abort on a warning.
    $ErrorActionPreference = 'Continue'
    $output = & $Python -I $Script @Arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw ("$([IO.Path]::GetFileName($Script)) failed (exit $LASTEXITCODE): " + ($output -join "`n")) }
    return $output
}

# The four README Quick start builders, each on the previous step's pack, in a temp folder.
# The pack replaces the installed one only when all four succeed.
function Build-Sm2Pack {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)]$Game, [Parameter(Mandatory)][string]$StateRoot)
    $b = $Package.Builders; $paks = $Game.Paks; $py = $Package.Python
    $name = 'SM2VR_FirstPerson_Test.pak'
    $work = Join-Path $StateRoot ('pack-build-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $steps = 'camera', 'diagnostics', 'menus', 'controls'
    foreach ($s in $steps) { New-Item -ItemType Directory -Force -Path (Join-Path $work $s) | Out-Null }
    $out = @{}; foreach ($s in $steps) { $out[$s] = Join-Path (Join-Path $work $s) $name }
    try {
        Invoke-Sm2Python $py (Join-Path $b 'build_first_person.py') @('--camera-only', '--source', (Join-Path $paks 'default_other.pak'), '--preset', (Join-Path $b 'config\first_person_wide_fov.json'), '--out', $out.camera) | Out-Null
        Invoke-Sm2Python $py (Join-Path $b 'build_vr_diagnostics.py') @('--camera-pack', $out.camera, '--presets-pack', (Join-Path $paks 'default_presets.pak'), '--out', $out.diagnostics) | Out-Null
        Invoke-Sm2Python $py (Join-Path $b 'build_vr_menus.py') @('--base-pack', $out.diagnostics, '--source', (Join-Path $paks 'default_other.pak'), '--preset', (Join-Path $b 'config\vr_menus.json'), '--out', $out.menus) | Out-Null
        Invoke-Sm2Python $py (Join-Path $b 'build_vr_controls.py') @('--base', $out.menus, '--source', (Join-Path $paks 'default_other.pak'), '--out', $out.controls) | Out-Null
        if (-not (Test-Path -LiteralPath $out.controls -PathType Leaf)) { throw 'The last pack builder wrote no pack.' }
        # Chase camera pack: the menus pack without its first-person camera files (docs/THIRD_PERSON.md).
        # Built inside the same folder so both packs replace the installed ones together.
        $chase = Join-Path (Join-Path (Join-Path $work 'controls') 'chase') $name
        New-Item -ItemType Directory -Force -Path (Split-Path $chase) | Out-Null
        Invoke-Sm2Python $py (Join-Path $b 'build_chase_cam.py') @('--base', $out.menus, '--out', $chase) | Out-Null
        if (-not (Test-Path -LiteralPath $chase -PathType Leaf)) { throw 'The chase camera builder wrote no pack.' }
        $packDir = Join-Path $StateRoot 'pack'
        if (Test-Path -LiteralPath $packDir) { Remove-Item -LiteralPath $packDir -Recurse -Force }
        Move-Item -LiteralPath (Join-Path $work 'controls') -Destination $packDir
        $pak = Join-Path $packDir $name
        $chasePak = Join-Path (Join-Path $packDir 'chase') $name
        return [pscustomobject]@{ path = $pak; sha256 = (Get-Sm2Sha256 $pak); chase = [pscustomobject]@{ path = $chasePak; sha256 = (Get-Sm2Sha256 $chasePak) } }
    } finally {
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

# Grenade aim hooks the game only on the equipment layout it was written for (see run-modded.ps1).
function Test-Sm2GrenadeLayout {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)]$Game)
    try {
        Invoke-Sm2Python $Package.Python (Join-Path $Package.Builders 'inspect_grenade_equipment_layout.py') @((Join-Path $Game.Paks 'default_ssl.pak')) | Out-Null
        return $true
    } catch { return $false }
}

# Copies the game's save and settings folder (not its 600+ MB shader cache in client\).
function Backup-Sm2Saves {
    param([Parameter(Mandatory)][string]$StateRoot, [string]$SaberRoot = (Join-Path $env:LOCALAPPDATA 'Saber\Space Marine 2'), [int]$Keep = 10)
    $copied = @()
    $backups = Join-Path $StateRoot 'save-backups'
    $target = Join-Path $backups (Get-Date -Format 'yyyyMMdd_HHmmss')
    foreach ($part in 'storage', 'server') {
        $source = Join-Path $SaberRoot $part
        if (Test-Path -LiteralPath $source -PathType Container) {
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            Copy-Item -LiteralPath $source -Destination (Join-Path $target $part) -Recurse -Force
            $copied += $part
        }
    }
    if (-not $copied.Count) { return $null }
    $old = @(Get-ChildItem -LiteralPath $backups -Directory | Sort-Object Name -Descending | Select-Object -Skip $Keep)
    foreach ($d in $old) { Remove-Item -LiteralPath $d.FullName -Recurse -Force }
    return $target
}

# Full install or repair. Returns the new install state. Preflight is the caller's job.
function Install-Sm2 {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)]$Game, [string]$StateRoot = (Get-Sm2StateRoot),
        [string]$SaberRoot = (Join-Path $env:LOCALAPPDATA 'Saber\Space Marine 2'), [scriptblock]$Progress = { param($m) })
    New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
    $previous = Read-Sm2InstallState $StateRoot
    & $Progress 'Backing up your saves'
    $backup = Backup-Sm2Saves -StateRoot $StateRoot -SaberRoot $SaberRoot
    & $Progress 'Copying the mod settings'
    $kept = Install-Sm2Config -Package $Package -StateRoot $StateRoot
    & $Progress 'Building the VR data pack from your game files'
    $pack = Build-Sm2Pack -Package $Package -Game $Game -StateRoot $StateRoot
    & $Progress 'Checking the grenade layout'
    $grenade = Test-Sm2GrenadeLayout -Package $Package -Game $Game
    # Files a crashed launch left in the game folder stay recorded until launch cleanup removes them.
    $gameFiles = @()
    if ($previous -and $previous.game_files) { $gameFiles = @($previous.game_files) }
    $state = [ordered]@{
        schema = 1
        mod_version = $Package.Version.mod_version
        package_root = $Package.Root
        game_root = $Game.Root
        exe_timestamp = (Get-Sm2PeTimestamp $Game.Exe)
        pack = $pack
        grenade_layout_verified = $grenade
        last_save_backup = $backup
        preserved_config = @($kept)
        bindings_confirmed_sha256 = $(if ($previous) { $previous.bindings_confirmed_sha256 } else { $null })
        game_files = $gameFiles
        installed_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    Write-Sm2InstallState $StateRoot $state
    return [pscustomobject]$state
}

# Whether a controls pack carries the native button layout this DLL sends: INPUT_LAYOUT in
# scripts/build_vr_controls.py, in the report beside the pak. Otherwise interact lands on the trackpad
# and grenades on Right B, with nothing in the log to say why. A release can replace the DLL at the
# same mod version, so the version check alone doesn't catch it.
function Test-Sm2PackLayout {
    param([Parameter(Mandatory)][string]$Pak)
    $expected = 2
    $report = [IO.Path]::ChangeExtension($Pak, '.json')
    if (-not (Test-Path -LiteralPath $report)) { return $false }
    try { return ([IO.File]::ReadAllText($report) | ConvertFrom-Json).input_layout -eq $expected } catch { return $false }
}

# Reasons the install must be redone before playing; empty when it is current.
function Get-Sm2InstallProblems {
    param($InstallState, [Parameter(Mandatory)]$Package, [Parameter(Mandatory)]$Game)
    $problems = @()
    if (-not $InstallState) { return @('not installed') }
    if ($InstallState.mod_version -ne $Package.Version.mod_version) { $problems += "installed for mod $($InstallState.mod_version), this is $($Package.Version.mod_version)" }
    if ($InstallState.game_root -ne $Game.Root) { $problems += 'the game folder changed' }
    if ($InstallState.exe_timestamp -ne (Get-Sm2PeTimestamp $Game.Exe)) { $problems += 'the game updated since install' }
    if (-not $InstallState.pack -or -not (Test-Path -LiteralPath $InstallState.pack.path) -or (Get-Sm2Sha256 $InstallState.pack.path) -ne $InstallState.pack.sha256) { $problems += 'the data pack is missing or changed' }
    elseif (-not (Test-Sm2PackLayout $InstallState.pack.path)) { $problems += 'the data pack was built for an older version of the mod' }
    elseif (-not $InstallState.pack.chase -or -not (Test-Path -LiteralPath $InstallState.pack.chase.path) -or (Get-Sm2Sha256 $InstallState.pack.chase.path) -ne $InstallState.pack.chase.sha256) { $problems += 'the chase camera pack is missing or changed' }
    return $problems
}

# One administrator prompt that lets this Windows user modify the game's bin and mods folders.
function Grant-Sm2GameFolderAccess {
    param([Parameter(Mandatory)]$Game)
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $commands = @()
    if (-not (Test-Path -LiteralPath $Game.Mods)) { $commands += "mkdir `"$($Game.Mods)`"" }
    foreach ($dir in @($Game.Bin, $Game.Mods)) { $commands += "icacls `"$dir`" /grant `"$($user):(OI)(CI)M`"" }
    $p = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/c', ($commands -join ' && ')) -Verb RunAs -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode -eq 0
}
