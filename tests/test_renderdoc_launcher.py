"""Actual PowerShell ownership functions, with every process API replaced."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from installer_support import write_game_stub
from pwsh import pwsh

ROOT = Path(__file__).resolve().parents[1]


class RenderDocLauncherTests(unittest.TestCase):
    def run_case(self, case):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            root = Path(temporary).resolve()
            game = root / 'game bin' / 'Game.exe'
            tool = root / 'tool bin' / 'renderdoccmd.exe'
            for path in (game, tool):
                path.parent.mkdir()
                path.write_bytes(b'not executable: all process operations are mocked')
            script = r'''
$ErrorActionPreference='Stop'
. 'FUNCTIONS'
$global:case='CASE';$global:clock=[DateTime]::Parse('2026-10-20T00:00:00Z').ToUniversalTime()
$global:starts=@();$global:kills=@();$global:waits=@();$global:ids=@();$global:cims=0;$global:started=$false;$global:incomplete=$false;$global:missing=0
$gamePath='GAME';$toolPath='TOOL';$prefix='PREFIX'
function New-FakeProcess($id,$path,$created,$kind) {
 $p=[pscustomobject]@{Id=$id;StartTime=$created;HasExited=$false;Handle=123;MainModule=[pscustomobject]@{FileName=$path};Kind=$kind;Refreshes=0;Disposed=$false;ExitCode=7}
 $p | Add-Member ScriptMethod Refresh {
  $this.Refreshes++
  if($this.Kind -eq 'helper') {
   if($global:case -eq 'helper_exit'){$this.HasExited=$true}
   if($global:case -eq 'helper_changed' -and $this.Refreshes -ge 2){$this.StartTime=$this.StartTime.AddSeconds(1)}
  } elseif($global:case -eq 'child_exit'){$this.HasExited=$true}
  elseif($global:case -eq 'child_reused' -and $this.Refreshes -ge 2){$this.StartTime=$this.StartTime.AddSeconds(1)}
  elseif($global:case -eq 'child_path_changed'){$this.MainModule.FileName+='other'}
 }
 $p | Add-Member ScriptMethod Kill {$global:kills+=$this.Id;$this.HasExited=$true}
 $p | Add-Member ScriptMethod WaitForExit {param($milliseconds);$global:waits+=@{id=$this.Id;ms=$milliseconds};if($this.Kind -eq 'helper' -and $global:case -eq 'helper_normal_exit'){$this.HasExited=$true};return $this.HasExited}
 $p | Add-Member ScriptMethod Dispose {$this.Disposed=$true}
 $p | Add-Member ScriptMethod CloseMainWindow {throw 'unexpected UI automation'}
 return $p
}
$global:helper=New-FakeProcess 8111 $toolPath $global:clock 'helper'
$global:child=New-FakeProcess 9222 $gamePath ($global:clock.AddMilliseconds(1)) 'game'
function Get-Date {return $global:clock}
function Start-Sleep {param($Milliseconds);$global:clock=$global:clock.AddMilliseconds($Milliseconds)}
function Get-Process {param($Name,$Id,$ErrorAction)
 if($PSBoundParameters.ContainsKey('Id')){if($global:incomplete){throw 'An incomplete CIM candidate was adopted'};$global:ids+=$Id;return $global:child}
 if($global:case -eq 'preexisting'){return [pscustomobject]@{HasExited=$false;Id=9999}}
}
function Get-CimInstance {param($ClassName,$OperationTimeoutSec)
 $global:cims++
 if($OperationTimeoutSec -ne 3){throw 'unbounded CIM operation'}
 if($global:case -eq 'late_discovery' -and $global:cims -gt 1){$global:clock=$global:clock.AddSeconds(16)}
 if($global:case -eq 'no_child'){return}
 $created=$global:clock.Date.AddMilliseconds(1)
 # Snapshot data is independent of each retained Process object.
 $created=[DateTime]::Parse('2026-10-20T00:00:00.001Z').ToUniversalTime()
 $parent=8111;$path=$gamePath
 if($global:case -eq 'wrong_parent'){$parent=9999}
 if($global:case -eq 'wrong_path'){$path+='other'}
 if($global:case -eq 'old_creation'){$created=$created.AddSeconds(-1)}
 if($global:case -eq 'child_creation_mismatch'){$created=$created.AddMilliseconds(1)}
 if($global:case -eq 'changed_provenance' -and $global:cims -gt 1){$parent=9999}
 if(($global:case -in @('missing_once','missing_then_complete_late') -and $global:cims -eq 1) -or $global:case -in @('missing_forever','missing_wrong_parent','missing_old_creation','missing_past_deadline')){$path=$null}
 if($global:case -eq 'missing_wrong_parent'){$parent=9999}
 if($global:case -eq 'missing_old_creation'){$created=$created.AddSeconds(-1)}
 if($global:case -eq 'missing_past_deadline'){$global:clock=$global:clock.AddSeconds(16)}
 if($global:case -eq 'missing_then_complete_late' -and $global:cims -eq 2){$global:clock=$global:clock.AddSeconds(16)}
 $global:incomplete=-not $path;if($global:incomplete){$global:missing++}
 [pscustomobject]@{Name='Game.exe';ProcessId=9222;ParentProcessId=$parent;ExecutablePath=$path;CreationDate=$created}
 if($global:case -eq 'ambiguous'){[pscustomobject]@{Name='Game.exe';ProcessId=9333;ParentProcessId=8111;ExecutablePath=$gamePath;CreationDate=$created}}
}
function Start-Process {[CmdletBinding()]param($FilePath,$WorkingDirectory,$ArgumentList,[switch]$PassThru,$WindowStyle,$RedirectStandardOutput,$RedirectStandardError)
 $global:starts+=@{file=$FilePath;directory=$WorkingDirectory;arguments=@($ArgumentList);pass=[bool]$PassThru;window=$WindowStyle;stdout=$RedirectStandardOutput;stderr=$RedirectStandardError;steam=$env:SteamAppId;mod=$env:SM2VR_CAPTURE_FIXTURE;proxy_bridge=$env:SM2VR_RENDERDOC_PROXY_BRIDGE}
 $global:started=$true
 if($global:case -eq 'start_failure'){throw 'mock capture start failed'}
 if($FilePath -eq $toolPath){return $global:helper};return $global:child
}
$env:SteamAppId='old';Remove-Item Env:SteamGameId -ErrorAction SilentlyContinue;$env:SM2VR_CAPTURE_FIXTURE='preserved';$env:SM2VR_RENDERDOC_PROXY_BRIDGE='original'
$lease=$null;$owned=$null;$failure=$null;$cleanupFailure=$null
if($global:case -eq 'existing_log'){[IO.File]::WriteAllText($prefix+'.stdout.log','must survive')}
try {
 $options=@{Executable=$gamePath;ArgumentList=@('--startup','--set','/Config/Foo','true')}
 if($global:case -ne 'normal'){$options.CaptureTool=$toolPath;$options.CapturePrefix=$prefix;$options.CaptureHelper=[ref]$lease}
 $owned=Start-Sm2OwnedGame @options
 if($global:case -ne 'leave'){[void](Stop-Sm2OwnedGame -Process $owned -NoUiAutomation);$owned.Dispose()}
} catch {$failure=$_.Exception.Message}
finally {if($lease -and $global:case -ne 'leave'){try{[void](Stop-Sm2CaptureHelper $lease)}catch{$cleanupFailure=$_.Exception.Message}}}
@{failure=$failure;cleanup_failure=$cleanupFailure;owned=($null -ne $owned);starts=@($global:starts);kills=@($global:kills);waits=@($global:waits);ids=@($global:ids);cims=$global:cims;missing=$global:missing;child_disposed=$global:child.Disposed;helper_disposed=$global:helper.Disposed;lease=($null -ne $lease);exit_code=$(if($lease){$lease.ExitCode}else{$null});proxy_bridge=$env:SM2VR_RENDERDOC_PROXY_BRIDGE;existing_log=$(if($global:case -eq 'existing_log'){[IO.File]::ReadAllText($prefix+'.stdout.log')}else{$null});elapsed=($global:clock-[DateTime]::Parse('2026-10-20T00:00:00Z').ToUniversalTime()).TotalSeconds;steam=$env:SteamAppId;game_env=$env:SteamGameId} | ConvertTo-Json -Compress -Depth 6
'''
            for name, value in {'FUNCTIONS': ROOT / 'scripts/owned-game-process.ps1',
                                'CASE': case, 'GAME': game, 'TOOL': tool,
                                'PREFIX': root / 'frame capture'}.items():
                script = script.replace(name, str(value).replace("'", "''"))
            harness = root / 'fixture.ps1'
            harness.write_text(script)
            result = subprocess.run([pwsh(), '-NoProfile', '-File', str(harness)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout.splitlines()[-1])

    def test_normal_launch_is_unchanged(self):
        value = self.run_case('normal')
        self.assertIsNone(value['failure']); self.assertTrue(value['owned'])
        self.assertEqual(value['kills'], [9222]); self.assertEqual(value['cims'], 0)
        self.assertFalse(value['lease']); self.assertEqual(len(value['starts']), 1)
        self.assertEqual(value['starts'][0]['proxy_bridge'], 'original')
        self.assertEqual(value['starts'][0]['arguments'], ['--startup', '--set', '/Config/Foo', 'true'])

    def test_transient_missing_cim_path_waits_without_adopting_until_fresh_complete_identity(self):
        value = self.run_case('missing_once')
        self.assertIsNone(value['failure']); self.assertTrue(value['owned'])
        self.assertEqual(value['missing'], 1); self.assertEqual(value['cims'], 3)
        self.assertEqual(value['elapsed'], 0.1); self.assertEqual(value['ids'], [9222])
        self.assertEqual(value['kills'], [9222, 8111])

    def test_persistent_missing_path_expires_without_game_handle_or_adoption(self):
        value = self.run_case('missing_forever')
        self.assertIn('within 15 seconds', value['failure']); self.assertIn('path remained unavailable', value['failure'])
        self.assertFalse(value['owned']); self.assertEqual(value['ids'], [])
        self.assertEqual(value['elapsed'], 15); self.assertEqual(value['kills'], [8111])

    def test_missing_path_does_not_defer_known_parent_or_creation_refusal(self):
        for case in ('missing_wrong_parent', 'missing_old_creation'):
            with self.subTest(case=case):
                value = self.run_case(case)
                self.assertIn('unqualified', value['failure']); self.assertEqual(value['elapsed'], 0)
                self.assertEqual(value['ids'], []); self.assertEqual(value['kills'], [8111])

    def test_incomplete_query_finishing_after_deadline_cannot_adopt_or_extend_wait(self):
        value = self.run_case('missing_past_deadline')
        self.assertIn('within 15 seconds', value['failure']); self.assertFalse(value['owned'])
        self.assertEqual(value['ids'], []); self.assertEqual(value['elapsed'], 16)
        self.assertEqual(value['cims'], 1); self.assertEqual(value['kills'], [8111])

    def test_complete_query_after_missing_path_cannot_acquire_a_game_handle_past_deadline(self):
        value = self.run_case('missing_then_complete_late')
        self.assertIn('within 15 seconds', value['failure']); self.assertFalse(value['owned'])
        self.assertEqual(value['ids'], []); self.assertEqual(value['elapsed'], 16.1)
        self.assertEqual(value['missing'], 1); self.assertEqual(value['cims'], 2)
        self.assertEqual(value['kills'], [8111])

    def test_exact_child_is_retained_and_both_owned_processes_clean_up(self):
        value = self.run_case('valid')
        self.assertIsNone(value['failure']); self.assertIsNone(value['cleanup_failure'])
        self.assertTrue(value['owned']); self.assertEqual(value['ids'], [9222])
        self.assertEqual(value['kills'], [9222, 8111]); self.assertTrue(value['child_disposed']); self.assertTrue(value['helper_disposed'])
        launch = value['starts'][0]
        self.assertTrue(launch['file'].endswith('renderdoccmd.exe')); self.assertEqual(launch['window'], 'Hidden')
        self.assertEqual(launch['arguments'][:3], ['"capture"', '"-w"', '"-d"'])
        self.assertEqual(launch['arguments'][4], '"-c"')
        self.assertIn('game bin', launch['arguments'][3]); self.assertIn('frame capture', launch['arguments'][5])
        self.assertEqual(launch['arguments'][-4:], ['"--startup"', '"--set"', '"/Config/Foo"', '"true"'])
        self.assertEqual(launch['steam'], '2183900'); self.assertEqual(launch['mod'], 'preserved')
        self.assertEqual(launch['proxy_bridge'], '1'); self.assertEqual(value['proxy_bridge'], 'original')
        self.assertTrue(Path(launch['stdout']).is_absolute()); self.assertTrue(launch['stderr'].endswith('.stderr.log'))
        self.assertEqual(value['exit_code'], 7)
        self.assertEqual(value['steam'], 'old'); self.assertIsNone(value['game_env'])
        self.assertIn({'id': 8111, 'ms': 5000}, value['waits'])

    def test_ambiguous_or_unqualified_child_is_never_adopted_or_killed(self):
        for case in ('ambiguous', 'wrong_parent', 'wrong_path', 'old_creation'):
            with self.subTest(case=case):
                value = self.run_case(case)
                self.assertIsNotNone(value['failure']); self.assertFalse(value['owned'])
                self.assertEqual(value['ids'], []); self.assertEqual(value['kills'], [8111])

    def test_retention_rechecks_child_identity_and_parent(self):
        for case in ('child_creation_mismatch', 'child_reused', 'child_path_changed', 'changed_provenance', 'child_exit', 'late_discovery'):
            with self.subTest(case=case):
                value = self.run_case(case)
                self.assertIsNotNone(value['failure']); self.assertFalse(value['owned'])
                self.assertTrue(value['child_disposed']); self.assertEqual(value['kills'], [8111])

    def test_live_exact_helper_is_required(self):
        value = self.run_case('helper_exit')
        self.assertIsNotNone(value['failure']); self.assertFalse(value['owned'])
        self.assertEqual(value['ids'], []); self.assertEqual(value['kills'], []); self.assertTrue(value['helper_disposed'])
        value = self.run_case('helper_changed')
        self.assertIsNotNone(value['failure']); self.assertIsNotNone(value['cleanup_failure'])
        self.assertFalse(value['owned']); self.assertEqual(value['kills'], [])

    def test_preexisting_game_and_start_failure_never_adopt(self):
        value = self.run_case('preexisting')
        self.assertEqual(value['failure'], 'The game is already running.'); self.assertEqual(value['starts'], [])
        value = self.run_case('start_failure')
        self.assertEqual(value['failure'], 'mock capture start failed'); self.assertFalse(value['lease'])
        self.assertEqual(value['kills'], []); self.assertEqual(value['steam'], 'old'); self.assertIsNone(value['game_env'])

    def test_no_child_timeout_and_helper_wait_are_bounded(self):
        value = self.run_case('no_child')
        self.assertIn('within 15 seconds', value['failure']); self.assertEqual(value['elapsed'], 15)
        self.assertEqual(value['kills'], [8111]); self.assertFalse(value['owned'])
        value = self.run_case('helper_normal_exit')
        self.assertIsNone(value['failure']); self.assertEqual(value['kills'], [9222]); self.assertTrue(value['helper_disposed'])

    def test_no_close_leaves_only_the_retained_launch_running(self):
        value = self.run_case('leave')
        self.assertIsNone(value['failure']); self.assertTrue(value['owned']); self.assertTrue(value['lease'])
        self.assertEqual(value['kills'], []); self.assertFalse(value['child_disposed']); self.assertFalse(value['helper_disposed'])

    def test_existing_capture_diagnostics_are_never_overwritten(self):
        value = self.run_case('existing_log')
        self.assertIsNotNone(value['failure']); self.assertEqual(value['starts'], [])
        self.assertEqual(value['existing_log'], 'must survive'); self.assertEqual(value['kills'], [])

    def test_headset_wrapper_forwards_capture_only_when_requested(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            scripts = Path(temporary) / 'scripts'; scripts.mkdir()
            shutil.copyfile(ROOT / 'scripts/run-ordinary-headset.ps1', scripts / 'run-ordinary-headset.ps1')
            (scripts / 'run-modded.ps1').write_text('''
param([int]$Seconds,[string]$Tag,[switch]$NoUiAutomation,[string]$DataPak,[string]$GameArgs,[switch]$RenderDocCapture,$LaunchFlags)
@{capture=[bool]$RenderDocCapture;ui_disabled=[bool]$NoUiAutomation;args=$GameArgs;stereo=$env:SM2VR_STEREO} | ConvertTo-Json -Compress
''')
            for enabled in (False, True):
                command = [pwsh(), '-NoProfile', '-File', str(scripts / 'run-ordinary-headset.ps1'), '-DirectLaunch']
                if enabled: command.append('-RenderDocCapture')
                result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                value = json.loads(result.stdout)
                self.assertEqual(value['capture'], enabled); self.assertTrue(value['ui_disabled'])
                self.assertEqual(value['stereo'], '1'); self.assertIn('enableModdersAutostartFlow', value['args'])

    def test_missing_capture_tool_refuses_before_deployment_or_process_apis(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            scripts = Path(temporary) / 'scripts'; scripts.mkdir()
            shutil.copyfile(ROOT / 'scripts/run-modded.ps1', scripts / 'run-modded.ps1')
            write_game_stub(Path(temporary))
            result = subprocess.run([pwsh(), '-NoProfile', '-File', str(scripts / 'run-modded.ps1'),
                                     '-NoUiAutomation', '-RenderDocCapture'], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0); self.assertIn('Portable RenderDoc is missing', result.stderr)
            self.assertNotIn('installed ', result.stdout)

    def test_windows_argv_empty_terminal_slash_and_refused_controls(self):
        script = ". '" + str(ROOT / 'scripts/owned-game-process.ps1') + "'\n" + r'''
$rejected=0
foreach($value in @('a"b',"a`nb","a`rb",("a"+[char]0+"b"))){try{ConvertTo-Sm2CaptureArgument $value | Out-Null}catch{$rejected++}}
@{empty=(ConvertTo-Sm2CaptureArgument '');trailing=(ConvertTo-Sm2CaptureArgument 'C:\directory\');rejected=$rejected} | ConvertTo-Json -Compress
'''
        result = subprocess.run([pwsh(), '-NoProfile', '-Command', script], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value, dict(empty='""', trailing='"C:\\directory\\\\"', rejected=4))


if __name__ == '__main__':
    unittest.main()
