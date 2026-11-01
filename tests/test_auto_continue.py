from pathlib import Path
import json
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/auto-continue.ps1'


def run(body):
    # Windows PowerShell, which every Windows machine has: this file uses nothing from PowerShell 7,
    # and drive-game.ps1 runs it under powershell.exe.
    command = f". '{SCRIPT}'\n{body}"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', command],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


class AutoContinueTests(unittest.TestCase):
    def test_presses_on_schedule_until_the_first_stereo_pair(self):
        presses = run('''
$s = New-Sm2AutoContinue -FirstAt 15 -Interval 5
$out = @()
foreach ($t in 0, 10, 15, 16, 20, 24, 25) { $out += Step-Sm2AutoContinue $s '' $t }
$out += Step-Sm2AutoContinue $s '[I] OrdinaryHeadset: consumed=1 submitted=1' 30
$out += Step-Sm2AutoContinue $s '' 35
@{presses=$out; count=$s.Presses; done=$s.Done} | ConvertTo-Json -Compress''')
        self.assertEqual(presses['presses'], ['none', 'none', 'enter', 'none', 'enter', 'none', 'enter', 'none', 'none'])
        self.assertEqual(presses['count'], 3)
        self.assertTrue(presses['done'], 'no Enter may reach gameplay once stereo has started')

    def test_in_game_keys_count_from_the_first_stereo_pair(self):
        state = run('''
$s = New-Sm2AutoContinue -FirstAt 0 -Interval 5
$null = Step-Sm2AutoContinue $s '' 0
$null = Step-Sm2AutoContinue $s '[I] OrdinaryHeadset: consumed=1 submitted=1' 42.5
$keys = @(ConvertTo-Sm2KeySchedule -Keys 'esc@10, DOWN@12' -Offset $s.InGameAt -Known 'ESC','DOWN','ENTER')
$bad = try { ConvertTo-Sm2KeySchedule -Keys 'F12@1' -Known 'ESC'; 'accepted' } catch { 'refused' }
@{at=$s.InGameAt; names=@($keys.Name); times=@($keys.At); bad=$bad; none=@(ConvertTo-Sm2KeySchedule -Keys '' -Known 'ESC').Count} | ConvertTo-Json -Compress''')
        self.assertEqual(state, {'at': 42.5, 'names': ['ESC', 'DOWN'], 'times': [52.5, 54.5], 'bad': 'refused', 'none': 0})

    def test_stops_after_the_press_limit(self):
        state = run('''
$s = New-Sm2AutoContinue -FirstAt 0 -Interval 1 -MaxPresses 3
$n = 0; foreach ($t in 0..10) { if ((Step-Sm2AutoContinue $s '' $t) -ne 'none') { $n++ } }
@{n=$n; done=$s.Done} | ConvertTo-Json -Compress''')
        self.assertEqual(state, {'n': 3, 'done': True})

    def test_down_once_on_the_main_menu_after_the_first_load(self):
        # The 23 Oct sequence: title 0x1400, 0x1800, load 0x800, 0x0, then the main menu 0x1400.
        log = lambda flags: f'[09:11:29.000] [I] [tid:1] CinematicState: available=true scripted=false paused=false published={flags} requested={flags} samples=1'
        presses = run(f'''
$s = New-Sm2AutoContinue -FirstAt 0 -Interval 1
$out = @()
$out += Step-Sm2AutoContinue $s '{log('0x1400')}' 0
$out += Step-Sm2AutoContinue $s '{log('0x1800')}' 1
$out += Step-Sm2AutoContinue $s '{log('0x800')}' 2
$out += Step-Sm2AutoContinue $s '{log('0x0')}' 3
$out += Step-Sm2AutoContinue $s '{log('0x1400')}' 4
$out += Step-Sm2AutoContinue $s '' 5
$out += Step-Sm2AutoContinue $s '{log('0xc00')}' 6
$out | ConvertTo-Json -Compress''')
        self.assertEqual(presses, ['enter', 'enter', 'enter', 'enter', 'down-enter', 'enter', 'enter'],
                         'the title 0x1400 gets plain Enter; the main menu gets one Down only')

    def test_down_once_when_the_main_menu_never_publishes_its_flags(self):
        # The 12:10 sequence: title 0x1400, 0xc00, 0x1800, then 0x800 for good with the main menu on screen.
        log = lambda flags: f'[12:11:29.000] [I] [tid:1] CinematicState: available=true scripted=false paused=false published={flags} requested={flags} samples=1'
        presses = run(f'''
$s = New-Sm2AutoContinue -FirstAt 0 -Interval 5 -StuckSeconds 90
$out = @()
$out += Step-Sm2AutoContinue $s '{log('0x1400')}' 0
$out += Step-Sm2AutoContinue $s '{log('0xc00')}' 5
$out += Step-Sm2AutoContinue $s '{log('0x1800')}' 10
$out += Step-Sm2AutoContinue $s '{log('0x800')}' 15
foreach ($t in 20, 25, 30, 35, 40) {{ $out += Step-Sm2AutoContinue $s '{log('0x800')}' $t }}
$out | ConvertTo-Json -Compress''')
        # That 0x800 was the main menu (as in every 25 Oct run): the Down goes at its fifth sample
        # (before that the menu can still be coming up and ignores it), not after 90 s, and only once.
        self.assertEqual(presses, ['enter'] * 7 + ['down-enter', 'enter'])

    def test_down_on_the_main_menu_when_the_dialog_logged_only_0xc00(self):
        # 25 Oct 16:09 launch: 0x1400 eleven times, one 0xc00, then the main menu's 0x800 (no 0x1800).
        log = lambda flags: f'[16:11:29.000] [I] [tid:1] CinematicState: available=true scripted=false paused=false published={flags} requested={flags} samples=1'
        presses = run(f'''
$s = New-Sm2AutoContinue -FirstAt 0 -Interval 5
$out = @()
$out += Step-Sm2AutoContinue $s '{log('0x1400')}' 0
$out += Step-Sm2AutoContinue $s '{log('0xc00')}' 5
foreach ($t in 10, 15, 20, 25, 30, 35) {{ $out += Step-Sm2AutoContinue $s '{log('0x800')}' $t }}
$out | ConvertTo-Json -Compress''')
        self.assertEqual(presses, ['enter'] * 6 + ['down-enter', 'enter'])

    def test_down_after_the_stuck_time_when_no_dialog_was_seen(self):
        log = lambda flags: f'[12:11:29.000] [I] [tid:1] CinematicState: available=true scripted=false paused=false published={flags} requested={flags} samples=1'
        presses = run(f'''
$s = New-Sm2AutoContinue -FirstAt 0 -Interval 5 -StuckSeconds 90
$out = @()
$out += Step-Sm2AutoContinue $s '{log('0x0')}' 0
foreach ($t in 5, 10, 50, 94) {{ $out += Step-Sm2AutoContinue $s '{log('0x800')}' $t }}
$out += Step-Sm2AutoContinue $s '{log('0x800')}' 99
$out | ConvertTo-Json -Compress''')
        self.assertEqual(presses, ['enter'] * 5 + ['down-enter'], 'without the dialog, 90 s of 0x800 still gets the Down')

    def test_log_tail_reads_only_new_text_while_the_file_is_open_for_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'sm2vr.log'
            tails = run(f'''
$path = '{log}'
$w = [IO.File]::Open($path, 'Create', 'Write', 'ReadWrite, Delete')
$s = New-Sm2AutoContinue
$b = [Text.Encoding]::UTF8.GetBytes("first`n"); $w.Write($b, 0, $b.Length); $w.Flush()
$one = Read-Sm2LogTail $s $path
$b = [Text.Encoding]::UTF8.GetBytes("second`n"); $w.Write($b, 0, $b.Length); $w.Flush()
$two = Read-Sm2LogTail $s $path
$w.Dispose()
@{{one=$one; two=$two; missing=(Read-Sm2LogTail (New-Sm2AutoContinue) ($path + '.none'))}} | ConvertTo-Json -Compress''')
        self.assertEqual(tails, {'one': 'first\n', 'two': 'second\n', 'missing': ''})


if __name__ == '__main__':
    unittest.main()
