@echo off
rem Starts the SM2VR window. Windows marks files from a downloaded zip as "from the internet" and
rem PowerShell then refuses them, so they are unblocked first. Nothing here needs administrator.
if not exist "%~dp0app\Sm2VrApp.ps1" (
    echo SM2VR can't start from inside the zip. Extract the whole zip to a folder, then run SM2VR.cmd there.
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0.' -Recurse -File | Unblock-File"
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0app\Sm2VrApp.ps1"
