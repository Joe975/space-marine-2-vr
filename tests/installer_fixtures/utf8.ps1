# Fixture output is read by Python as UTF-8 (paths in tests are not ASCII).
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
# Load the module's script functions as well as the utility cmdlets already in the session.
# Some Windows PowerShell hosts otherwise discover ConvertFrom-Json but not Get-FileHash.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psm1') -Force -Global
