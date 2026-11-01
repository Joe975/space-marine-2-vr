# Builds the player package build\installer\SM2VR-<version>\ and its zip. Nothing is published.
#
# Usage: package-installer.ps1 [-Version 0.1.0] [-Dll path] [-NoZip]
#   -Dll defaults to build\mod\bin\dxgi.dll (in a worktree: the main checkout's). A release build
#   should come from build-mod.bat on the commit being packaged; publish-release.ps1 does that.
#   The release targets the game build installed on this machine ("latest only").
param([string]$Version = '', [string]$Dll = '', [string]$OutDirectory = '', [switch]$NoZip,
    [string]$ManifestUrl = 'https://raw.githubusercontent.com/Joe975/space-marine-2-vr/main/installer/manifest.json')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'installer-package.ps1')
. (Join-Path $repo 'installer\app\sm2-game.ps1')
# Build outputs and refs/ live in the main checkout; worktrees share them.
$main = $repo
try { $common = git -C $repo rev-parse --path-format=absolute --git-common-dir; if ($common) { $main = Split-Path -Parent $common } } catch {}
function Resolve-Shared([string]$Relative) {
    foreach ($base in @($repo, $main)) { $p = Join-Path $base $Relative; if (Test-Path -LiteralPath $p) { return $p } }
    throw "Missing $Relative (looked in $repo and $main)."
}
if (-not $Version) { $Version = ([IO.File]::ReadAllText((Join-Path $repo 'installer\VERSION'))).Trim() }
if (-not $Dll) { $Dll = Resolve-Shared 'build\mod\bin\dxgi.dll' }
if (-not $OutDirectory) { $OutDirectory = Join-Path $repo 'build\installer' }
$game = Find-Sm2Game
if (-not $game) { throw 'Space Marine 2 is not installed here; the release targets the installed game build.' }
$commit = ''; try { $commit = (git -C $repo rev-parse HEAD).Trim(); if (git -C $repo status --porcelain) { $commit += '-dirty' } } catch {}
$root = New-Sm2Package -Repo $repo -Version $Version -OutDirectory $OutDirectory -Dll $Dll `
    -OpenVrDll (Resolve-Shared 'refs\vrframework-core\shipping\openvr_api.dll') `
    -GameExeTimestamp (Get-Sm2PeTimestamp $game.Exe) -GameBuildId $game.BuildId -PythonCache (Join-Path $main 'build\tools\python-embed') `
    -ManifestUrl $ManifestUrl -SourceCommit $commit
"package: $root"
if (-not $NoZip) {
    $zip = New-Sm2PackageZip $root
    "zip: $($zip.Path)"
    "sha256: $($zip.Sha256)"
    "size: $($zip.Size)"
}
