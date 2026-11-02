# Release packaging functions used by package-installer.ps1 and publish-release.ps1 (dev machine
# only). Layout: docs/INSTALLER_PLAN.md "Package layout".

# python.org embeddable CPython, pinned. Also checked for a valid PSF Authenticode signature.
$Sm2PythonZipName = 'python-3.13.3-embed-amd64.zip'
$Sm2PythonZipUrl = 'https://www.python.org/ftp/python/3.13.3/python-3.13.3-embed-amd64.zip'
$Sm2PythonZipSha256 = '59ff76e16e6597de47474fb22be69e7191a89116910d728ab735079b078e52db'

# Gesture tuning the F11 overlay saves, the head placement, world scale and the camera guard's
# settings (play space walking among them): the player's, kept across updates.
$Sm2PreservedConfig = @('index.calibration', 'index_swing.calibration', 'index_holster.calibration', 'index_guard.calibration',
    'index_grenade.calibration', 'index_grenade_aim.calibration', 'index_support.calibration', 'index_guard_guide.calibration',
    'index_camera_guard.calibration', 'index_camera_modes.calibration')

# Calibrations the player profile loads: named in installer\app\sm2-profile.ps1, or read by the DLL
# beside index.calibration (Sm2Probe.cpp: camera guard, support, HUD, native HUD/prompt/corners, aim dot,
# menu, viewer play). Hand-calibration candidates and baselines in config\ stay out (release tidy, 26 Oct).
$Sm2ShippedCalibrations = 'index.calibration', 'index_camera_modes.calibration', 'index_camera_guard.calibration',
    'openxr_index_grip.calibration', 'index_hands_accepted.calibration', 'index_weapon.calibration', 'index_gaze_camera.calibration',
    'index_guard_guide.calibration', 'index_swing.calibration', 'index_holster.calibration', 'index_support.calibration',
    'index_hud.calibration', 'index_native_hud.calibration', 'index_native_prompt.calibration', 'index_native_hud_corners.calibration', 'index_aim_dot.calibration', 'index_hud_reticle.calibration',
    'index_menu.calibration', 'index_viewer_play.calibration'

$Sm2AppParts = 'Sm2VrApp', 'sm2-game', 'sm2-profile', 'sm2-preflight', 'sm2-install', 'sm2-launch', 'sm2-update', 'sm2-report', 'sm2-view'
$Sm2Builders = 'build_first_person.py', 'build_vr_diagnostics.py', 'build_vr_menus.py', 'build_vr_controls.py', 'build_chase_cam.py', 'inspect_grenade_equipment_layout.py'
$Sm2BuilderPresets = 'first_person_wide_fov.json', 'vr_menus.json'

# License texts the binaries' third-party code requires in a release (installer\licenses\<name>.txt,
# indexed in installer\THIRD_PARTY_NOTICES.txt). python.txt comes from the embeddable zip.
$Sm2ThirdPartyLicenses = 'vrframework-core', 'reframework', 'openvr', 'openxr-sdk', 'bddisasm', 'directxtk', 'directxtk12',
    'fidelityfx', 'glm', 'imgui', 'imguizmo', 'kananlib', 'minhook', 'safetyhook', 'spdlog', 'streamline', 'xbyak', 'zydis', 'nlohmann-json'

# The game exe builds the DLL knows: the reference build plus every GameBuildTable.inc target.
function Get-Sm2SupportedBuilds([string]$Repo) {
    $builds = [ordered]@{}
    $header = [IO.File]::ReadAllText((Join-Path $Repo 'mod\src\GameBuild.h'))
    if ($header -match 'REFERENCE_EXE\s*=\s*(0x[0-9a-fA-F]{8})') { $builds[$Matches[1].ToLowerInvariant()] = '2026-10-05' }
    foreach ($m in [regex]::Matches([IO.File]::ReadAllText((Join-Path $Repo 'mod\src\GameBuildTable.inc')), '\{Image::exe,0x[0-9a-fA-F]{8},(0x[0-9a-fA-F]{8}),0x[0-9a-fA-F]+,"([0-9-]+)"')) {
        $builds[$m.Groups[1].Value.ToLowerInvariant()] = $m.Groups[2].Value
    }
    return $builds
}

# Downloads (once, into build\tools) and unpacks the embeddable Python after checking hash and signature.
function Install-Sm2PackagePython {
    param([Parameter(Mandatory)][string]$Cache, [Parameter(Mandatory)][string]$Destination)
    $zip = Join-Path $Cache $Sm2PythonZipName
    if (-not (Test-Path -LiteralPath $zip)) {
        New-Item -ItemType Directory -Force -Path $Cache | Out-Null
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $Sm2PythonZipUrl -OutFile $zip -UseBasicParsing
    }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $Sm2PythonZipSha256) { throw "Python embeddable zip hash mismatch: $hash" }
    Expand-Archive -LiteralPath $zip -DestinationPath $Destination
    foreach ($binary in 'python.exe', 'python313.dll') {
        $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $Destination $binary)
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Python Software Foundation') { throw "$binary is not validly signed by the PSF ($($sig.Status))." }
    }
}

# Problems that make a package unfit to ship; empty when it passes.
function Test-Sm2PackageAudit([string]$Root) {
    $problems = @()
    foreach ($item in 'SM2VR.cmd', 'README.txt', 'version.json', 'app\Sm2VrApp.ps1', 'app\lib\owned-game-process.ps1', 'mod\dxgi.dll',
        'mod\openvr_api.dll', 'mod\launch.json', 'mod\config\vr_actions.json', 'mod\config\bindings_index.json', 'builders\build_vr_controls.py',
        'builders\build_chase_cam.py', 'python\python.exe', 'docs\CONTROLS.html', 'THIRD_PARTY_NOTICES.txt', 'licenses\python.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $item))) { $problems += "missing $item" }
    }
    foreach ($name in $Sm2ThirdPartyLicenses) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root "licenses\$name.txt"))) { $problems += "missing license: $name" }
    }
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative = $file.FullName.Substring($Root.Length).TrimStart('\')
        # Game data never ships; players build the pack from their own game files.
        if ($file.Extension -eq '.pak' -or $file.Name -like 'default_*') { $problems += "game data: $relative" }
        if ($file.Extension -in '.dmp', '.mdmp', '.log', '.jsonl') { $problems += "run output: $relative" }
        if ($file.Length -gt 40MB) { $problems += "too large ($([int]($file.Length / 1MB)) MB): $relative" }
        if ($file.Extension -in '.ps1', '.cmd') {
            $bytes = [IO.File]::ReadAllBytes($file.FullName)
            # Windows PowerShell 5.1 reads BOM-less scripts as ANSI; keep them ASCII.
            if (@($bytes | Where-Object { $_ -gt 127 }).Count) { $problems += "not ASCII: $relative" }
        }
    }
    $version = Join-Path $Root 'version.json'
    if (Test-Path -LiteralPath $version) {
        $v = [IO.File]::ReadAllText($version) | ConvertFrom-Json
        # preserve may name files the release does not ship (guard, grenade): the F11 overlay
        # creates them, and Install-Sm2Config never touches a file it has no copy of.
        foreach ($field in 'mod_version', 'game_exe_timestamp', 'game_build_date', 'manifest_url') { if (-not $v.$field) { $problems += "version.json has no $field" } }
    }
    return $problems
}

function Copy-Sm2Files([string[]]$Names, [string]$From, [string]$To) {
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    foreach ($n in $Names) { Copy-Item -LiteralPath (Join-Path $From $n) -Destination (Join-Path $To $n) }
}

# Assembles the portable folder. Returns its path. -GameExeTimestamp is the build the release is
# for ("latest only": the build on the packaging machine).
function New-Sm2Package {
    param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$OutDirectory,
        [Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$OpenVrDll,        [Parameter(Mandatory)][string]$GameExeTimestamp, [string]$GameBuildId, [Parameter(Mandatory)][string]$PythonCache,
        [Parameter(Mandatory)][string]$ManifestUrl, [string]$SourceCommit = '')
    $supported = Get-Sm2SupportedBuilds $Repo
    $timestamp = $GameExeTimestamp.ToLowerInvariant()
    if (-not $supported.Contains($timestamp)) { throw "The installed game ($timestamp) is not a build the DLL knows ($(@($supported.Keys) -join ', ')). Port the DLL first." }
    $root = Join-Path $OutDirectory "SM2VR-$Version"
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $installer = Join-Path $Repo 'installer'
    # cmd.exe and Notepad want CRLF whatever the checkout's line endings are.
    foreach ($name in 'SM2VR.cmd', 'README.txt') {
        $text = [IO.File]::ReadAllText((Join-Path $installer $name)) -replace "`r?`n", "`r`n"
        [IO.File]::WriteAllText((Join-Path $root $name), $text, (New-Object Text.ASCIIEncoding))
    }
    Copy-Sm2Files ($Sm2AppParts | ForEach-Object { "$_.ps1" }) (Join-Path $installer 'app') (Join-Path $root 'app')
    Copy-Sm2Files @('owned-game-process.ps1') (Join-Path $Repo 'scripts') (Join-Path $root 'app\lib')
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'mod\config') | Out-Null
    Copy-Item -LiteralPath $Dll -Destination (Join-Path $root 'mod\dxgi.dll')
    Copy-Item -LiteralPath $OpenVrDll -Destination (Join-Path $root 'mod\openvr_api.dll')
    Copy-Item -LiteralPath (Join-Path $installer 'launch.json') -Destination (Join-Path $root 'mod\launch.json')
    $config = Join-Path $Repo 'config'
    Copy-Sm2Files ($Sm2ShippedCalibrations + @('vr_actions.json', 'bindings_index.json')) $config (Join-Path $root 'mod\config')
    Copy-Sm2Files $Sm2Builders (Join-Path $Repo 'scripts') (Join-Path $root 'builders')
    Copy-Sm2Files $Sm2BuilderPresets $config (Join-Path $root 'builders\config')
    Install-Sm2PackagePython -Cache $PythonCache -Destination (Join-Path $root 'python')
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'docs'), (Join-Path $root 'licenses') | Out-Null
    # The bundled interpreter builds the controls page, which also proves it runs the stdlib builders.
    $py = Join-Path $root 'python\python.exe'
    & $py -I (Join-Path $Repo 'scripts\build_controls_page.py') --source (Join-Path $Repo 'docs\CONTROLS.md') --out (Join-Path $root 'docs\CONTROLS.html') --version $Version
    if ($LASTEXITCODE -ne 0) { throw 'Building the controls page with the bundled Python failed.' }
    Copy-Item -LiteralPath (Join-Path $root 'python\LICENSE.txt') -Destination (Join-Path $root 'licenses\python.txt')
    Copy-Sm2Files ($Sm2ThirdPartyLicenses | ForEach-Object { "$_.txt" }) (Join-Path $installer 'licenses') (Join-Path $root 'licenses')
    $notices = [IO.File]::ReadAllText((Join-Path $installer 'THIRD_PARTY_NOTICES.txt')) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $root 'THIRD_PARTY_NOTICES.txt'), $notices, (New-Object Text.ASCIIEncoding))
    $versionInfo = [ordered]@{
        mod_version = $Version
        game_exe_timestamp = $timestamp
        game_build_date = $supported[$timestamp]
        game_build_id = $GameBuildId
        preserve = $Sm2PreservedConfig
        manifest_url = $ManifestUrl
        source_commit = $SourceCommit
        dll_sha256 = (Get-FileHash -LiteralPath $Dll -Algorithm SHA256).Hash.ToLowerInvariant()
        built_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    [IO.File]::WriteAllText((Join-Path $root 'version.json'), ($versionInfo | ConvertTo-Json -Depth 3), (New-Object Text.UTF8Encoding $false))
    $problems = @(Test-Sm2PackageAudit $root)
    if ($problems.Count) { throw ("Package audit failed:`n" + ($problems -join "`n")) }
    return $root
}

function New-Sm2PackageZip([string]$PackageRoot) {
    $zip = $PackageRoot + '.zip'
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -LiteralPath $PackageRoot -DestinationPath $zip -CompressionLevel Optimal
    $file = Get-Item -LiteralPath $zip
    return [pscustomobject]@{ Path = $zip; Sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant(); Size = $file.Length }
}
