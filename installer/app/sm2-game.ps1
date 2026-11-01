# Finds the Space Marine 2 install through Steam (app 2183900) and describes its layout.
# Windows PowerShell 5.1 compatible: players run this, not only the dev machine.
# Every system read takes an override so tests can use fixture trees.

$Sm2AppId = '2183900'
$Sm2ExeName = 'Warhammer 40000 Space Marine 2 - Retail'

# Minimal Valve KeyValues (VDF/ACF) reader: quoted keys, quoted values, nested braces.
function Read-Sm2Vdf([string]$Text) {
    $tokens = [regex]::Matches($Text, '"((?:[^"\\]|\\.)*)"|([{}])')
    $root = @{}; $stack = New-Object System.Collections.Stack; $current = $root; $key = $null
    foreach ($t in $tokens) {
        if ($t.Groups[2].Success) {
            if ($t.Groups[2].Value -eq '{') {
                if ($null -eq $key) { throw 'VDF: block without a key.' }
                $child = @{}; $current[$key] = $child; $stack.Push($current); $current = $child; $key = $null
            } else {
                if ($stack.Count -eq 0) { throw 'VDF: unbalanced closing brace.' }
                $current = $stack.Pop()
            }
            continue
        }
        $value = $t.Groups[1].Value -replace '\\\\', '\'
        if ($null -eq $key) { $key = $value } else { $current[$key] = $value; $key = $null }
    }
    if ($stack.Count -ne 0) { throw 'VDF: unclosed block.' }
    return $root
}

function Get-Sm2SteamPath {
    foreach ($location in @(@('HKCU:\Software\Valve\Steam', 'SteamPath'), @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath'))) {
        try {
            $value = (Get-ItemProperty -LiteralPath $location[0] -Name $location[1] -ErrorAction Stop).($location[1])
            if ($value) { return ($value -replace '/', '\') }
        } catch {}
    }
    return $null
}

function Get-Sm2GameLayout([string]$Root) {
    $client = Join-Path $Root 'client_pc\root'
    $bin = Join-Path $client 'bin\pc'
    [pscustomobject]@{
        Root = $Root
        ClientRoot = $client
        Bin = $bin
        Exe = Join-Path $bin ($Sm2ExeName + '.exe')
        Paks = Join-Path $client 'paks\client\default'
        Mods = Join-Path $client 'mods'
        BuildId = $null
        Library = $null
    }
}

# Returns the game layout, or $null when Steam has no install of the game.
# -Override (or SM2VR_GAME_ROOT) skips Steam entirely, for unusual setups.
function Find-Sm2Game {
    param([string]$SteamPath, [string]$Override = $env:SM2VR_GAME_ROOT)
    if ($Override) {
        $layout = Get-Sm2GameLayout $Override
        if (-not (Test-Path -LiteralPath $layout.Exe -PathType Leaf)) { throw "SM2VR_GAME_ROOT has no game exe: $($layout.Exe)" }
        return $layout
    }
    if (-not $SteamPath) { $SteamPath = Get-Sm2SteamPath }
    if (-not $SteamPath -or -not (Test-Path -LiteralPath $SteamPath -PathType Container)) { return $null }
    $libraries = New-Object System.Collections.Generic.List[string]
    $libraries.Add($SteamPath)
    $folders = Join-Path $SteamPath 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $folders) {
        $vdf = Read-Sm2Vdf ([IO.File]::ReadAllText($folders))
        $block = $vdf['libraryfolders']
        if ($block) {
            foreach ($name in ($block.Keys | Sort-Object)) {
                $entry = $block[$name]
                if ($entry -is [hashtable] -and $entry['path']) { $libraries.Add(($entry['path'] -replace '/', '\')) }
            }
        }
    }
    $seen = @{}
    foreach ($library in $libraries) {
        $id = $library.TrimEnd('\').ToLowerInvariant()
        if ($seen.ContainsKey($id)) { continue }
        $seen[$id] = $true
        $manifest = Join-Path $library ("steamapps\appmanifest_$Sm2AppId.acf")
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { continue }
        $state = (Read-Sm2Vdf ([IO.File]::ReadAllText($manifest)))['AppState']
        if (-not $state -or -not $state['installdir']) { continue }
        $layout = Get-Sm2GameLayout (Join-Path $library ("steamapps\common\" + $state['installdir']))
        if (-not (Test-Path -LiteralPath $layout.Exe -PathType Leaf)) { continue }
        $layout.BuildId = $state['buildid']
        $layout.Library = $library
        return $layout
    }
    return $null
}

# The PE header TimeDateStamp identifies the game build (mod/src/GameBuildTable.inc keys on it).
function Get-Sm2PeTimestamp([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $Path" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "No PE signature: $Path" }
        $stream.Position = $peOffset + 8
        return ('0x{0:x8}' -f $reader.ReadUInt32())
    } finally { $stream.Dispose() }
}
