@echo off
rem Starts Space Marine 2 directly, without the Easy Anti-Cheat bootstrapper.
rem Offline / solo use only: the game treats this as a modded session and public matchmaking is off.
rem Steam must be running. Approved by Joe 2026-10-11 (see STATUS.md).
setlocal
set "GAME_BIN=F:\Steam\steamapps\common\Space Marine 2\client_pc\root\bin\pc"
set SteamAppId=2183900
set SteamGameId=2183900
cd /d "%GAME_BIN%"
start "" "Warhammer 40000 Space Marine 2 - Retail.exe" %*
