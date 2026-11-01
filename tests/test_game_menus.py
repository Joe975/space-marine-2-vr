"""scripts/game-menus.ps1: what drive-game.ps1 knows about the menus. Runs under Windows PowerShell
(powershell.exe, which every Windows machine has), so it does not skip without pwsh."""
from pathlib import Path
import json
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[1]


def run(body):
    command = f". '{REPO / 'scripts/game-menus.ps1'}'\n{body}"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', command],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


def flags(value):
    return f'[15:18:07.561] [I] [tid:1] CinematicState: available=true scripted=false paused=false published={value} requested={value} samples=1'


class GameMenuTests(unittest.TestCase):
    def test_pause_menu_item_needs_one_more_down_than_its_index(self):
        # 25 Oct: four Downs opened Photo mode; Quit to title screen is the fifth.
        keys = run("@{quit=@(Get-Sm2PauseMenuKeys 'QuitToTitle'); first=@(Get-Sm2PauseMenuKeys 'Continue');"
                   " checkpoint=@(Get-Sm2PauseMenuKeys 'QuitToTitle' -Checkpoint); load=@(Get-Sm2PauseMenuKeys 'LoadLastCheckpoint' -Checkpoint);"
                   " bad=$(try { Get-Sm2PauseMenuKeys 'ReturnToBarge'; 'accepted' } catch { 'refused' })} | ConvertTo-Json -Compress")
        # 30 Oct hangar menu (no checkpoint): Quit to title is the fourth Down; the fifth is Quit.
        self.assertEqual(keys['quit'], ['ESC'] + ['DOWN'] * 4)
        self.assertEqual(keys['checkpoint'], ['ESC'] + ['DOWN'] * 5)
        self.assertEqual(keys['load'], ['ESC', 'DOWN', 'DOWN'])
        self.assertEqual(keys['first'], ['ESC', 'DOWN'])
        self.assertEqual(keys['bad'], 'refused', 'an item not read off a screen is not guessed')

    def test_in_game_once_the_player_has_been_present_for_the_settle_time(self):
        # 25 Oct 15:38: without stereo the hangar publishes 0x800, so only the player's body tells.
        body = lambda actor: f'[15:38:02.769] [I] [tid:1] BodyFollowing: enabled=true actor={actor} body=20386588300 context=1'
        out = run(f'''
$s = New-Sm2InGame -SettleSeconds 15
$r = @()
$r += Step-Sm2InGame $s '{body('0')}' 0           # menus: no player
$r += Step-Sm2InGame $s '{flags('0x800')}' 5      # flags alone never count
$r += Step-Sm2InGame $s '{body('20476fa12c0')}' 10
$r += Step-Sm2InGame $s '{body('0')}' 15          # lost again (a load): start over
$r += Step-Sm2InGame $s '{body('20476fa12c0')}' 20
$r += Step-Sm2InGame $s '' 34
$r += Step-Sm2InGame $s '{body('20476fa12c0')}' 35
$r | ConvertTo-Json -Compress''')
        self.assertEqual(out, [False, False, False, False, False, False, True])

    def test_continue_retries_enter_then_down_and_never_two_downs_after_a_start(self):
        body = '[15:56:02.769] [I] [tid:1] BodyFollowing: enabled=true actor=20476fa12c0 body=1 context=1'
        stuck = run(f'''
$c = New-Sm2ContinueCheck -Since 100 -RetrySeconds 15
@((Step-Sm2ContinueCheck $c '{flags('0x800')}' 105), (Step-Sm2ContinueCheck $c '' 115), (Step-Sm2ContinueCheck $c '{flags('0x800')}' 120),
  (Step-Sm2ContinueCheck $c '' 130), (Step-Sm2ContinueCheck $c '' 135), (Step-Sm2ContinueCheck $c '' 150)) | ConvertTo-Json -Compress''')
        # 15:55: Continue ignored, 0x800 for good. Enter at +15 s, Down+Enter at +30 s, give up at +45 s.
        self.assertEqual(stuck, ['wait', 'enter', 'wait', 'down-enter', 'wait', 'give-up'])
        started = run(f'''
$c = New-Sm2ContinueCheck -Since 100 -RetrySeconds 15
@((Step-Sm2ContinueCheck $c '' 116), (Step-Sm2ContinueCheck $c '{flags('0x0')}' 120), (Step-Sm2ContinueCheck $c '' 200)) | ConvertTo-Json -Compress''')
        self.assertEqual(started, ['enter', 'started', 'started'], 'once it starts, no more keys')
        player = run(f'''
$c = New-Sm2ContinueCheck -Since 0
@((Step-Sm2ContinueCheck $c '{body}' 3), (Step-Sm2ContinueCheck $c '' 100)) | ConvertTo-Json -Compress''')
        self.assertEqual(player, ['started', 'started'])

    def test_vr_session_reads_the_last_status(self):
        waiting = '[16:04:11.971] [I] [tid:1] VrSession: status=OpenXR runtime: SteamVR/OpenXR; recommended eye 2468x2740 (max 8192x8192); waiting for session readiness epoch=5'
        ready = '[15:48:37.698] [I] [tid:1] VrSession: status=OpenXR tracking active; session state=3 epoch=6 submitted_pairs=162'
        out = run(f'''@((Get-Sm2VrSession '{waiting}'), (Get-Sm2VrSession "{waiting}`n{ready}"), (Get-Sm2VrSession '{flags('0x0')}')) | ConvertTo-Json -Compress''')
        self.assertEqual(out, ['waiting', 'ready', ''])

    def test_exit_watch_reads_the_watchdog_and_the_title(self):
        out = run(f'''
$w = New-Sm2ExitWatch -Since 0
@(
  (Step-Sm2ExitWatch $w '{flags('0x908')}' 1),
  (Step-Sm2ExitWatch $w '[15:18:37.288] [E] [tid:52120] HangWatchdog: no frame presented for 10403 ms; thread dump written=true' 2),
  (Step-Sm2ExitWatch $w '{flags('0x800')}`n{flags('0x1400')}' 3)
) | ConvertTo-Json -Compress''')
        self.assertEqual(out, ['', 'hang', 'title'])

    def test_exit_watch_calls_a_hang_when_logging_and_the_window_both_stop(self):
        # 25 Oct 15:48: the startup stall had used the watchdog's one dump, so the exit freeze
        # logged nothing at all; the mod's 5 s status lines stopped and the window stopped responding.
        probe = '[15:48:42.713] [I] [tid:71508] Sm2Probe: presents=11215 fps=46.3 ui_draws=11215'
        out = run(f'''
$w = New-Sm2ExitWatch -Since 0 -SilentSeconds 20
$r = @()
$r += Step-Sm2ExitWatch $w '{probe}' 5 $true
$r += Step-Sm2ExitWatch $w '' 30 $true      # quiet but responding: a long load, not a hang
$r += Step-Sm2ExitWatch $w '{probe}' 31 $false
$r += Step-Sm2ExitWatch $w '' 50 $false     # 19 s of silence
$r += Step-Sm2ExitWatch $w '' 51 $false
$r | ConvertTo-Json -Compress''')
        self.assertEqual(out, ['', '', '', '', 'hang'])


if __name__ == '__main__':
    unittest.main()
