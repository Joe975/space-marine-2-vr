@echo off
rem Starts the SM2VR window. Windows marks files from a downloaded zip as "from the internet" and
rem PowerShell then refuses them, so they are unblocked first. Nothing here needs administrator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0.' -Recurse -File | Unblock-File"
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0app\Sm2VrApp.ps1"
