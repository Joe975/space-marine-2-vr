# One-button crash report: the newest run's logs plus install facts, as a zip the player can post.
# No memory dumps (they can hold anything); logs may contain the Windows user name, the UI says so.

# The fault pods hold copies of game heap memory, so they are dumps too.
$Sm2ReportExcluded = @('*.dmp', '*.mdmp', '*fault_pod.bin')
$Sm2ReportMaxFileBytes = 64MB

function Get-Sm2MachineFacts {
    $facts = [ordered]@{ os = $null; gpus = @(); steamvr_runtime = $null }
    try { $os = Get-CimInstance Win32_OperatingSystem; $facts.os = "$($os.Caption) $($os.Version)" } catch {}
    try { $facts.gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) driver $($_.DriverVersion)" }) } catch {}
    try { $facts.steamvr_runtime = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction Stop).ActiveRuntime } catch {}
    return $facts
}

function New-Sm2CrashReport {
    param([Parameter(Mandatory)]$Package, $Game, [string]$StateRoot = (Get-Sm2StateRoot), [Parameter(Mandatory)][string]$OutDirectory, $MachineFacts = (Get-Sm2MachineFacts))
    $runs = Join-Path $StateRoot 'runs'
    $run = $null
    if (Test-Path -LiteralPath $runs) { $run = Get-ChildItem -LiteralPath $runs -Directory | Sort-Object Name -Descending | Select-Object -First 1 }
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $staging = Join-Path $StateRoot ("report-$stamp")
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    try {
        $skipped = @()
        if ($run) {
            foreach ($file in Get-ChildItem -LiteralPath $run.FullName -Recurse -File) {
                $excluded = $false
                foreach ($pattern in $Sm2ReportExcluded) { if ($file.Name -like $pattern) { $excluded = $true } }
                if ($file.Length -gt $Sm2ReportMaxFileBytes) { $excluded = $true }
                if ($excluded) { $skipped += $file.Name; continue }
                $relative = $file.FullName.Substring($run.FullName.Length).TrimStart('\')
                $target = Join-Path (Join-Path $staging 'run') $relative
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
                Copy-Item -LiteralPath $file.FullName -Destination $target
            }
        }
        foreach ($f in @((Join-Path $StateRoot 'install.json'), (Join-Path $Package.Root 'version.json'), $Package.Profile)) {
            if (Test-Path -LiteralPath $f) { Copy-Item -LiteralPath $f -Destination $staging }
        }
        $report = [ordered]@{
            created_utc = (Get-Date).ToUniversalTime().ToString('o')
            mod_version = $Package.Version.mod_version
            dll_sha256 = $(if (Test-Path -LiteralPath $Package.Dll) { (Get-FileHash -LiteralPath $Package.Dll -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null })
            run = $(if ($run) { $run.Name } else { $null })
            skipped_files = @($skipped)
            game_build_id = $(if ($Game) { $Game.BuildId } else { $null })
            game_exe_timestamp = $(if ($Game) { try { Get-Sm2PeTimestamp $Game.Exe } catch { $null } } else { $null })
            machine = $MachineFacts
        }
        [IO.File]::WriteAllText((Join-Path $staging 'report.json'), ($report | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
        New-Item -ItemType Directory -Force -Path $OutDirectory | Out-Null
        $zip = Join-Path $OutDirectory ("SM2VR-report-$stamp.zip")
        Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip
        return $zip
    } finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}
