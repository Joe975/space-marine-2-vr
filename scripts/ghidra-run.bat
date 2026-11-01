@echo off
rem Runs a script from scripts\ghidra against the analysed Retail.exe (read-only, no re-analysis).
rem Usage: scripts\ghidra-run.bat Script.java [script args...]
rem (no angle brackets in rem lines: cmd parses them as redirection)
rem The project is build\ghidra\sm2, created once by analyzeHeadless -import (see STATUS.md).
setlocal
rem Ghidra rejects project paths containing "..", so resolve the repo root to a plain path.
for %%i in ("%~dp0..") do set "ROOT=%%~fi"
for /d %%d in ("%ROOT%\tools\jdk21\jdk-*") do set "JAVA_HOME=%%d"
set "GHIDRA_HEADLESS_MAXMEM=8G"
for /d %%d in ("%ROOT%\tools\ghidra\ghidra_*") do set "GHIDRA=%%d"
call "%GHIDRA%\support\analyzeHeadless.bat" "%ROOT%\build\ghidra" sm2 -process "Warhammer 40000 Space Marine 2 - Retail.exe" -noanalysis -readOnly -scriptPath "%ROOT%\scripts\ghidra" -postScript %*
