"""Shared helpers for the installer tests: run Windows PowerShell 5.1 (what players have) on a fixture."""
from pathlib import Path
import json
import subprocess

REPO = Path(__file__).resolve().parents[1]
APP = REPO / 'installer' / 'app'
FIXTURES = REPO / 'tests' / 'installer_fixtures'
TEMP = REPO / 'build' / 'test_temp'


def write_game_stub(root):
    """Launcher tests copy scripts/run-modded.ps1 into a temp tree; it looks the game up through
    installer/app/sm2-game.ps1, so give the tree a stub that returns the fixture's F: layout."""
    stub = Path(root) / 'installer' / 'app' / 'sm2-game.ps1'
    stub.parent.mkdir(parents=True, exist_ok=True)
    stub.write_text("function Find-Sm2Game { [pscustomobject]@{ Bin = 'F:\\Steam\\steamapps\\common\\Space Marine 2"
                    "\\client_pc\\root\\bin\\pc' } }\n", encoding='ascii')


def run_ps(script, *args, timeout=60):
    """Runs a .ps1 file under powershell.exe; returns CompletedProcess. Execution policy is bypassed
    the same way installer/SM2VR.cmd does it. Fixtures write UTF-8 (installer_fixtures/utf8.ps1)."""
    command = ['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
               '-File', str(script), *[str(a) for a in args]]
    return subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)


def run_ps_json(script, *args, timeout=60):
    result = run_ps(script, *args, timeout=timeout)
    if result.returncode != 0:
        raise AssertionError(f'{Path(script).name} failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
    return json.loads(result.stdout)
