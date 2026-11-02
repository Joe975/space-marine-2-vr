# Publishes a player release: clean build of the DLL, package, zip, GitHub release asset, manifest.
#
# Usage: publish-release.ps1 -Version 0.1.0 [-Channel stable|beta] [-Changelog 'text'] [-DryRun]
#   -DryRun does everything except upload and commit: build\installer\manifest.dry-run.json shows
#   the manifest that would be committed. Use it to test the pipeline.
#
# Publishing is refused until docs/DISTRIBUTION.md records the release decision: the doc must hold a
# line starting "Release authorized:" naming who decided and when, and the GitHub repo must be public.
param([Parameter(Mandatory)][string]$Version, [ValidateSet('stable', 'beta')][string]$Channel = 'stable', [string]$Changelog = '',
    [switch]$DryRun, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'installer-package.ps1')
. (Join-Path $repo 'installer\app\sm2-update.ps1')
$tag = "v$Version"
# The public repo; scripts/export_public.py builds its tree from this one.
$ghRepo = 'Joe975/space-marine-2-vr'

function Test-Sm2ReleaseAuthorized {
    param([string]$DistributionDoc, [string]$Visibility)
    $problems = @()
    $text = [IO.File]::ReadAllText($DistributionDoc)
    if ($text -notmatch '(?m)^Release authorized:[ \t]*\S') { $problems += 'docs/DISTRIBUTION.md has no "Release authorized:" line recording the release decision.' }
    if ($Visibility -ne 'PUBLIC') { $problems += "The GitHub repo is $Visibility, not PUBLIC." }
    return $problems
}

ConvertTo-Sm2Version $Version | Out-Null
if (git -C $repo status --porcelain) { throw 'Commit or stash changes first; a release is built from a clean commit.' }
if (-not $DryRun) {
    $visibility = (gh repo view $ghRepo --json visibility | ConvertFrom-Json).visibility
    $blocked = @(Test-Sm2ReleaseAuthorized -DistributionDoc (Join-Path $repo 'docs\DISTRIBUTION.md') -Visibility $visibility)
    if ($blocked.Count) { throw ("Publishing is not allowed yet:`n" + ($blocked -join "`n") + "`nUse -DryRun to test the pipeline.") }
}

$dll = ''
if (-not $SkipBuild) {
    # Release DLL from this commit, with the native tests build-mod.bat runs.
    & cmd.exe /c (Join-Path $PSScriptRoot 'build-mod.bat')
    if ($LASTEXITCODE -ne 0) { throw 'build-mod.bat failed.' }
    $dll = Join-Path $repo 'build\mod\bin\dxgi.dll'
}
$packageArgs = @{ Version = $Version; ManifestUrl = "https://raw.githubusercontent.com/$ghRepo/main/installer/manifest.json" }
if ($dll) { $packageArgs.Dll = $dll }
$output = & (Join-Path $PSScriptRoot 'package-installer.ps1') @packageArgs
$output
$zipPath = (($output | Where-Object { $_ -like 'zip: *' }) -replace '^zip: ', '')
$sha = (($output | Where-Object { $_ -like 'sha256: *' }) -replace '^sha256: ', '')
$size = [long](($output | Where-Object { $_ -like 'size: *' }) -replace '^size: ', '')
$packageVersion = [IO.File]::ReadAllText((Join-Path ($zipPath -replace '\.zip$', '') 'version.json')) | ConvertFrom-Json

$manifestPath = Join-Path $repo 'installer\manifest.json'
$manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
$entry = [pscustomobject]@{
    version = $Version
    url = "https://github.com/$ghRepo/releases/download/$tag/$([IO.Path]::GetFileName($zipPath))"
    sha256 = $sha
    size = $size
    game_exe_timestamp = $packageVersion.game_exe_timestamp
    game_build_date = $packageVersion.game_build_date
    changelog = $Changelog
    published_utc = (Get-Date).ToUniversalTime().ToString('o')
}
$manifest.channels | Add-Member -NotePropertyName $Channel -NotePropertyValue $entry -Force
$json = $manifest | ConvertTo-Json -Depth 5
# The installer must accept what was written.
Get-Sm2UpdatePlan -Manifest ($json | ConvertFrom-Json) -CurrentVersion '0.0.0' -Channel $Channel | Out-Null

if ($DryRun) {
    $dry = Join-Path $repo 'build\installer\manifest.dry-run.json'
    [IO.File]::WriteAllText($dry, $json, (New-Object Text.UTF8Encoding $false))
    "dry run: would upload $zipPath to release '$tag' and commit installer\manifest.json as $dry"
    return
}
gh release view $tag --repo $ghRepo *> $null
if ($LASTEXITCODE -ne 0) { gh release create $tag --repo $ghRepo --title "SM2VR $Version" --notes $Changelog }
gh release upload $tag $zipPath --repo $ghRepo --clobber
if ($LASTEXITCODE -ne 0) { throw 'Upload failed; the manifest was not changed.' }
[IO.File]::WriteAllText($manifestPath, $json, (New-Object Text.UTF8Encoding $false))
git -C $repo add installer/manifest.json
git -C $repo commit -m "installer: publish $Version ($Channel)"
"published $Version on $Channel. Export and push to $ghRepo main so the manifest URL serves it."
