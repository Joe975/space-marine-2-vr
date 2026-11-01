# -Mode plan: Get-Sm2UpdatePlan on a fixture manifest. -Mode apply: Install-Sm2Update from a zip.
# -Mode report: New-Sm2CrashReport for a fixture package/state.
param([string]$Mode, [string]$ManifestPath, [string]$CurrentVersion, [string]$Channel = 'stable', [string]$GameExeTimestamp = '',
    [string]$Zip, [string]$Sha256, [string]$PackageRoot, [string]$StateRoot, [string]$OutDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'utf8.ps1')
foreach ($part in 'sm2-game', 'sm2-install', 'sm2-update', 'sm2-report') { . (Join-Path $PSScriptRoot "..\..\installer\app\$part.ps1") }
switch ($Mode) {
    'plan' {
        $manifest = [IO.File]::ReadAllText($ManifestPath) | ConvertFrom-Json
        $plan = Get-Sm2UpdatePlan -Manifest $manifest -CurrentVersion $CurrentVersion -Channel $Channel -GameExeTimestamp $GameExeTimestamp
        @{ action = $plan.Action; message = $plan.Message; version = $(if ($plan.Release) { $plan.Release.version } else { $null }) } | ConvertTo-Json
    }
    'apply' { ConvertTo-Json -InputObject (Install-Sm2Update -Zip $Zip -Sha256 $Sha256 -PackageRoot $PackageRoot) }
    'report' { ConvertTo-Json -InputObject (New-Sm2CrashReport -Package (Get-Sm2Package $PackageRoot) -StateRoot $StateRoot -OutDirectory $OutDirectory -MachineFacts @{ os = 'fixture' }) }
}
