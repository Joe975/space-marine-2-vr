"""scripts/game-keys.ps1 and drive-game.ps1's third-person steps (F8/F9, hold, eyes). Runs under Windows
PowerShell (powershell.exe), so it does not skip without pwsh. Nothing here sends a key."""
from pathlib import Path
import json
import re
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[1]


def run(body):
    command = f". '{REPO / 'scripts/game-keys.ps1'}'\n{body}"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', command],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


class GameKeyTests(unittest.TestCase):
    def test_camera_keys_are_the_f8_and_f9_scan_codes(self):
        # Set 1 scan codes: F8 0x42, F9 0x43 (the mod's camera cycle and recenter, docs/THIRD_PERSON.md).
        codes = run('@{f8=$Sm2ScanCodes.F8; f9=$Sm2ScanCodes.F9; f11=$Sm2ScanCodes.F11} | ConvertTo-Json -Compress')
        self.assertEqual(codes, {'f8': 0x42, 'f9': 0x43, 'f11': 0x57})

    def test_hold_step_parses_key_and_bounded_seconds(self):
        out = run('''
$r = [ordered]@{}
$h = ConvertFrom-Sm2HoldStep 'w:6'; $r.key = $h.Key; $r.seconds = $h.Seconds
$r.fraction = (ConvertFrom-Sm2HoldStep 'S:0.5').Seconds
foreach ($bad in 'W', 'W:0', 'W:31', 'W:x', 'X:2') { $r[$bad] = try { $null = ConvertFrom-Sm2HoldStep $bad; 'accepted' } catch { 'refused' } }
$r | ConvertTo-Json -Compress''')
        self.assertEqual((out['key'], out['seconds'], out['fraction']), ('W', 6, 0.5))
        for bad in ('W', 'W:0', 'W:31', 'W:x', 'X:2'):
            self.assertEqual(out[bad], 'refused', bad)

    def test_driver_scripts_parse(self):
        # drive-game.ps1 has no unit-testable entry point; a parse error only shows at run time.
        names = ('drive-game.ps1', 'game-keys.ps1', 'game-menus.ps1', 'auto-continue.ps1', 'tour.ps1')
        listed = ','.join(f"'{REPO / 'scripts' / n}'" for n in names)
        out = run('$r = [ordered]@{}; foreach ($f in @(' + listed + ')) { $e = $null; '
                  '[void][System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$null, [ref]$e); '
                  '$r[(Split-Path -Leaf $f)] = @($e | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" }) }; '
                  '$r | ConvertTo-Json -Compress -Depth 3')
        for name in names:
            self.assertEqual(out[name] or [], [], name)

    def test_drive_game_has_hold_and_eyes_steps(self):
        drive = (REPO / 'scripts' / 'drive-game.ps1').read_text(encoding='utf-8')
        steps = set(re.findall(r"^\s+'([a-z-]+)' \{", drive, re.M))
        self.assertTrue({'hold', 'eyes'} <= steps, steps)
        # eyes names follow EyeDumpRequest's rule, so the mod never refuses one the driver accepts.
        self.assertIn("'^[A-Za-z0-9_-]{1,40}$'", drive)
        request = (REPO / 'mod' / 'src' / 'EyeDumpRequest.h').read_text(encoding='utf-8')
        self.assertIn('text.size()>40', request)


if __name__ == '__main__':
    unittest.main()
