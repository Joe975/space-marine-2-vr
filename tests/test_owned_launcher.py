from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
import json

from installer_support import write_game_stub
from pwsh import pwsh

ROOT=Path(__file__).resolve().parents[1]
VIDEO_JSON='{\n   "PlatformUserSettings": {\n      "Video": {\n         "renderResolutionPresetRef": {\n            "json_version": 1,\n            "uid": "ULTRA_PERFORMANCE"\n         },\n         "displayModeRef": null,\n         "resolutionWidthRef": null,\n         "resolutionHeightRef": null\n      }\n   }\n}\n'
VIDEO_OPTIONS="-Resolution 2560x1440 -WindowMode Windowed -RenderResolution Performance"

class OwnedLauncherTests(unittest.TestCase):
    def run_case(self,case,capture=False,video=False,pending=False):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);scripts=root/'scripts';scripts.mkdir();(root/'build/mod/bin').mkdir(parents=True);(root/'build/mod/bin/dxgi.dll').write_bytes(b'fixture')
            for name in ('run-modded.ps1','owned-game-process.ps1','game-video-settings.ps1','auto-continue.ps1','game-keys.ps1'):shutil.copyfile(ROOT/'scripts'/name,scripts/name)
            # A fake game settings tree; plain launches must leave it byte-identical.
            storage=root/'local/Saber/Space Marine 2/storage';(storage/'platform/config').mkdir(parents=True);(storage/'user/7656/MainLocal/config').mkdir(parents=True)
            (storage/'user/7656/MainLocal/config/platform_usr_settings.cfg').write_bytes(VIDEO_JSON.encode())
            for name in ('video.cfg','video_quality.cfg'):(storage/'platform/config'/name).write_bytes(b'SSF1'+name.encode())
            if pending:(root/'local/sm2vr').mkdir(parents=True);(root/'local/sm2vr/video-settings-pending.json').write_text('{}')
            for name in ('test-data-pak.ps1','runtime-api.ps1'):(scripts/name).write_text('# unused fixture')
            write_game_stub(root)
            if capture:
                tool=root/'build/tools/renderdoc-1.46/RenderDoc_1.46_64/renderdoccmd.exe';tool.parent.mkdir(parents=True);tool.write_bytes(b'mocked tool')
            fixture=r'''
$ErrorActionPreference='Stop'
$global:case='CASE';$global:capture=CAPTUREBOOL; $global:clock=[DateTime]::Parse('2026-10-20T00:00:00Z');$global:started=$false;$global:installed=$false;$global:killed=@();$global:refreshes=0;$global:disposed=$false;$global:launch=@{};$global:helperDisposed=$false
$env:LOCALAPPDATA='LOCALPATH';New-Item -ItemType Directory -Force -Path (Join-Path $env:LOCALAPPDATA 'sm2vr') | Out-Null
$env:SteamAppId='original-app';Remove-Item Env:SteamGameId -ErrorAction SilentlyContinue
$global:fake=[pscustomobject]@{Id=9222;StartTime=$(if($global:capture){$global:clock.AddMilliseconds(1)}else{$global:clock});HasExited=$false;WorkingSet64=1048576;MainWindowTitle='fixture';Responding=$true;Handle=1;MainModule=[pscustomobject]@{FileName='F:\Steam\steamapps\common\Space Marine 2\client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe'}}
$global:fake | Add-Member ScriptMethod Refresh { $global:refreshes++;if ($global:case -in @('early','foreign','reused') -and $global:refreshes -ge 1) { $this.HasExited=$true };if ($global:case -in @('stopped','lingering','lingering_foreign','unknown') -and $global:refreshes -ge 2) { $this.HasExited=$true };if($global:case -eq 'capture_early' -and $global:refreshes -ge 3){$this.HasExited=$true} }
$global:fake | Add-Member ScriptMethod Kill { $global:killed+=9222;$this.HasExited=$true }
$global:fake | Add-Member ScriptMethod WaitForExit { param($timeout);return $this.HasExited }
$global:fake | Add-Member ScriptMethod Dispose { $global:disposed=$true }
$global:fake | Add-Member ScriptProperty ExitCode {if($global:disposed){throw 'fixture was disposed before exit receipt'};if($global:case -eq 'exit_code_unavailable'){throw 'fixture exit code denied'};return -1073741819}
$global:fake | Add-Member ScriptMethod CloseMainWindow { throw 'NoUiAutomation invoked UI' }
$global:helper=[pscustomobject]@{Id=8111;StartTime=$global:clock;HasExited=$false;Handle=2;MainModule=[pscustomobject]@{FileName='TOOLPATH'};ExitCode=17}
$global:helper | Add-Member ScriptMethod Refresh {}
$global:helper | Add-Member ScriptMethod Kill {$global:killed+=8111;if($global:case -eq 'capture_helper_failure'){throw 'fixture helper would not stop'};$this.HasExited=$true}
$global:helper | Add-Member ScriptMethod WaitForExit {param($timeout);return $this.HasExited}
$global:helper | Add-Member ScriptMethod Dispose {$global:helperDisposed=$true}
function Get-Date { param($Format);if($Format){return $global:clock.ToString($Format)};return $global:clock }
function Start-Sleep { param($Milliseconds=0,$Seconds=0);$global:clock=$global:clock.AddMilliseconds($Milliseconds+1000*$Seconds) }
function Get-Process { param($Name,$Id,$ErrorAction)
 if($PSBoundParameters.ContainsKey('Id')){return $global:fake}
 if($Name -eq 'steam'){return [pscustomobject]@{Id=123}}
 if($global:capture -and $global:started -and -not $global:fake.HasExited){return $global:fake}
 if($global:case -in @('lingering','lingering_foreign','unknown') -and $global:started){
  if($global:case -eq 'unknown'){$unknown=[pscustomobject]@{Id=9999};$unknown | Add-Member ScriptProperty HasExited {throw 'fixture query denied'};return $unknown}
  $global:fake
  if($global:case -eq 'lingering_foreign'){[pscustomobject]@{Id=9999;HasExited=$false;WorkingSet64=999999999}}
  return
 }
 if(($global:case -in @('foreign','reused') -and $global:started) -or $global:case -eq 'preexisting'){return [pscustomobject]@{Id=$(if($global:case -eq 'reused'){9222}else{9999});HasExited=$false;WorkingSet64=999999999}}
}
function Get-CimInstance { param($ClassName);if($global:capture -and $global:started -and -not $global:fake.HasExited){return [pscustomobject]@{Name='Warhammer 40000 Space Marine 2 - Retail.exe';ProcessId=9222;ParentProcessId=$(if($global:case -in @('capture_foreign','capture_failure_locked_log')){9999}else{8111});ExecutablePath=$global:fake.MainModule.FileName;CreationDate=$global:fake.StartTime}};return @() }
function Start-Process { [CmdletBinding()]param($FilePath,$WorkingDirectory,$ArgumentList,[switch]$PassThru,$WindowStyle,$RedirectStandardOutput,$RedirectStandardError)
 $global:launch=@{file=$FilePath;working_directory=$WorkingDirectory;arguments=@($ArgumentList);pass_thru=[bool]$PassThru;app=$env:SteamAppId;game=$env:SteamGameId;mod=$env:SM2VR_LAUNCH_FIXTURE;output_size=$env:SM2VR_OUTPUT_SIZE;grenade_probe=$env:SM2VR_GRENADE_NATIVE_PROBE;grenade_aim=$env:SM2VR_GRENADE_AIM;grenade_layout=$env:SM2VR_GRENADE_EQUIPMENT_LAYOUT;video=([IO.File]::ReadAllText($global:usr) | ConvertFrom-Json).PlatformUserSettings.Video}
 if($global:case -eq 'failure'){throw 'fixture start failure'};$global:started=$true;if($global:case -eq 'capture_failure_locked_log'){[IO.File]::WriteAllText((Join-Path $env:LOCALAPPDATA 'sm2vr/sm2vr_fixture.log'),'locked fixture log')};if($global:capture){return $global:helper};return $global:fake
}
function cmd { throw 'The launcher invoked a blocking command shell' }
function Test-Path { param($Path,$LiteralPath,$PathType);$value=if($LiteralPath){$LiteralPath}else{$Path};if($value -like 'F:*'){if($value -like '*dxgi.dll'){return $global:installed};if($global:capture -and $value -like '*Retail.exe'){return $true};return $false};Microsoft.PowerShell.Management\Test-Path -LiteralPath $value }
function Copy-Item { param($Path,$Destination);if($Destination -like 'F:*dxgi.dll'){$global:installed=$true;return};throw 'Unexpected fixture copy' }
function Remove-Item { param($Path,$LiteralPath,[switch]$Force,$ErrorAction);$value=if($LiteralPath){$LiteralPath}else{$Path};if($value -like 'F:*dxgi.dll'){$global:installed=$false;return};Microsoft.PowerShell.Management\Remove-Item -Path $value -Force:$Force -ErrorAction SilentlyContinue }
if($global:case -eq 'capture_failure_locked_log') { function Move-Item {param($LiteralPath,$Destination,[switch]$Force);throw 'fixture locked log archive'} }
if($global:case -in @('capture_receipt_error','game_exit_receipt_error')) {
 function Set-Content {[CmdletBinding()]param($Path,[Parameter(ValueFromPipeline)]$Value,$LiteralPath)
  process {$destination=if($LiteralPath){$LiteralPath}else{$Path};if(($global:case -eq 'capture_receipt_error' -and $destination -like '*renderdoc-process.json') -or ($global:case -eq 'game_exit_receipt_error' -and $destination -like '*owned-game-exit.json')){throw 'fixture receipt I/O failure'};Microsoft.PowerShell.Management\Set-Content -LiteralPath $destination -Value $Value}
 }
}
if($global:case -eq 'grenade_layout_unverified'){$env:SM2VR_GRENADE_NATIVE_PROBE='1';$env:SM2VR_GRENADE_AIM='1'}
$env:SM2VR_LAUNCH_FIXTURE='preserved';ENVSIZE$global:failure=$null;$global:usr=Join-Path $env:LOCALAPPDATA 'Saber/Space Marine 2/storage/user/7656/MainLocal/config/platform_usr_settings.cfg'
try { & 'LAUNCHER' -Seconds 2 -Tag owned_fixture -NoUiAutomation -GameArgs '--startup --set /Config/Foo true' -NoClose:($global:case -eq 'leave') CAPTUREOPTION VIDEOOPTION | Out-Null } catch { $global:failure=$_.Exception.Message }
$receiptFile=Get-ChildItem -LiteralPath 'FIXTUREROOT/build/run' -Filter renderdoc-process.json -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$receipt=if($receiptFile){Get-Content -LiteralPath $receiptFile.FullName -Raw | ConvertFrom-Json}else{$null}
$gameExitFile=Get-ChildItem -LiteralPath 'FIXTUREROOT/build/run' -Filter owned-game-exit.json -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$gameExit=if($gameExitFile){Get-Content -LiteralPath $gameExitFile.FullName -Raw | ConvertFrom-Json}else{$null}
$launchFailureFile=Get-ChildItem -LiteralPath 'FIXTUREROOT/build/run' -Filter launch-failure.json -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$launchFailure=if($launchFailureFile){Get-Content -LiteralPath $launchFailureFile.FullName -Raw | ConvertFrom-Json}else{$null}
@{case=$global:case;started=$global:started;killed=@($global:killed);installed=$global:installed;disposed=$global:disposed;helper_disposed=$global:helperDisposed;receipt=$receipt;game_exit=$gameExit;launch_failure=$launchFailure;launch=$global:launch;elapsed=($global:clock-[DateTime]::Parse('2026-10-20T00:00:00Z')).TotalSeconds;video_json=[IO.File]::ReadAllText($global:usr);video_pending=[IO.File]::Exists((Join-Path $env:LOCALAPPDATA 'sm2vr/video-settings-pending.json'));app=$env:SteamAppId;game=$env:SteamGameId;failure=$global:failure} | ConvertTo-Json -Compress -Depth 5
'''.replace('CASE',case).replace('LOCALPATH',str(root/'local')).replace('LAUNCHER',str(scripts/'run-modded.ps1')).replace('CAPTUREBOOL','$true' if capture else '$false').replace('CAPTUREOPTION','-RenderDocCapture' if capture else '').replace('VIDEOOPTION',(video if isinstance(video,str) else VIDEO_OPTIONS) if video else '').replace('ENVSIZE',"$env:SM2VR_OUTPUT_SIZE='1944x2160';" if isinstance(video,str) and '-OutputSize' in video else '').replace('TOOLPATH',str(root/'build/tools/renderdoc-1.46/RenderDoc_1.46_64/renderdoccmd.exe')).replace('FIXTUREROOT',str(root))
            harness=root/'fixture.ps1';harness.write_text(fixture,encoding='utf8')
            result=subprocess.run([pwsh(),'-NoProfile','-File',str(harness)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            return json.loads(result.stdout.splitlines()[-1])

    def test_direct_start_preserves_arguments_environment_and_exact_owned_stop(self):
        value=self.run_case('deadline');self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[9222]);self.assertFalse(value['installed']);self.assertEqual(value['elapsed'],2)
        self.assertTrue(value['launch']['pass_thru']);self.assertEqual(value['launch']['app'],'2183900');self.assertEqual(value['launch']['game'],'2183900');self.assertEqual(value['launch']['mod'],'preserved');self.assertEqual(value['launch']['arguments'],['--startup','--set','/Config/Foo','true']);self.assertTrue(value['launch']['file'].endswith('Retail.exe'));self.assertEqual(value['app'],'original-app');self.assertIsNone(value['game'])

    def test_unverified_grenade_layout_turns_default_grenade_aim_off_instead_of_blocking_launch(self):
        # No Python/game layout in the fixture: the default-on aim must not abort the launch.
        value=self.run_case('grenade_layout_unverified');self.assertIsNone(value['failure']);self.assertTrue(value['started'])
        self.assertIsNone(value['launch']['grenade_probe']);self.assertIsNone(value['launch']['grenade_aim']);self.assertIsNone(value['launch']['grenade_layout'])
        self.assertFalse(value['installed'])

    def test_early_exit_is_observed_immediately_instead_of_waiting_deadline(self):
        value=self.run_case('early');self.assertIsNone(value['failure']);self.assertLess(value['elapsed'],2);self.assertEqual(value['killed'],[]);self.assertFalse(value['installed'])

    def test_foreign_game_is_never_adopted_stopped_or_unloaded(self):
        value=self.run_case('foreign');self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[]);self.assertTrue(value['installed'])
        # The launcher waits up to 30 s for a lingering game process before retaining files.
        self.assertGreaterEqual(value['elapsed'],30);self.assertLess(value['elapsed'],32)

    def test_externally_stopped_owned_process_reaches_cleanup_before_deadline(self):
        value=self.run_case('stopped');self.assertIsNone(value['failure']);self.assertEqual(value['elapsed'],1);self.assertEqual(value['killed'],[]);self.assertFalse(value['installed'])

    def test_reused_numeric_id_does_not_replace_retained_process_object(self):
        value=self.run_case('reused');self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[]);self.assertTrue(value['installed'])
        self.assertGreaterEqual(value['elapsed'],30);self.assertLess(value['elapsed'],32)

    def test_exited_owned_listing_does_not_masquerade_as_foreign_process(self):
        value=self.run_case('lingering');self.assertIsNone(value['failure']);self.assertEqual(value['elapsed'],1);self.assertEqual(value['killed'],[]);self.assertTrue(value['disposed']);self.assertFalse(value['installed'])

    def test_lingering_owned_plus_live_foreign_or_unknown_still_retains_files(self):
        for case in ('lingering_foreign','unknown'):
            with self.subTest(case=case):
                value=self.run_case(case);self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[]);self.assertTrue(value['disposed']);self.assertTrue(value['installed'])

    def test_preexisting_game_refuses_before_deployment_or_launch(self):
        value=self.run_case('preexisting');self.assertEqual(value['failure'],'The game is already running.');self.assertFalse(value['started']);self.assertFalse(value['installed']);self.assertEqual(value['killed'],[])

    def test_start_failure_restores_environment_and_cleans_owned_files(self):
        value=self.run_case('failure');self.assertEqual(value['failure'],'fixture start failure');self.assertEqual(value['killed'],[]);self.assertFalse(value['installed']);self.assertEqual(value['app'],'original-app');self.assertIsNone(value['game']);self.assertEqual(value['elapsed'],0)

    def test_no_close_retains_owned_game_and_files(self):
        value=self.run_case('leave');self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[]);self.assertTrue(value['installed'])

    def test_capture_launch_uses_retained_child_then_cleans_both_owned_processes(self):
        value=self.run_case('capture',capture=True);self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[9222,8111]);self.assertFalse(value['installed']);self.assertTrue(value['disposed']);self.assertTrue(value['helper_disposed'])
        self.assertTrue(value['launch']['file'].endswith('renderdoccmd.exe'));self.assertIn('"capture"',value['launch']['arguments'])
        self.assertEqual(value['receipt']['pid'],8111);self.assertEqual(value['receipt']['game_pid'],9222);self.assertEqual(value['receipt']['exit_code'],17)
        self.assertTrue(Path(value['receipt']['stdout']).is_absolute());self.assertTrue(value['receipt']['stderr'].endswith('.stderr.log'))
        self.assertEqual(value['game_exit'],dict(source='retained_game_handle',available=True,status='closed',pid=9222,created_utc='2026-10-20T00:00:00.001Z',exit_code=-1073741819))

    def test_capture_child_early_exit_cleans_helper_without_waiting_test_deadline(self):
        value=self.run_case('capture_early',capture=True);self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[8111]);self.assertLess(value['elapsed'],2);self.assertFalse(value['installed']);self.assertTrue(value['helper_disposed'])

    def test_capture_foreign_child_is_not_stopped_or_unloaded(self):
        value=self.run_case('capture_foreign',capture=True);self.assertIn('unqualified',value['failure']);self.assertEqual(value['killed'],[8111]);self.assertTrue(value['installed']);self.assertFalse(value['disposed']);self.assertTrue(value['helper_disposed'])

    def test_capture_no_close_keeps_game_helper_and_files(self):
        value=self.run_case('leave',capture=True);self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[]);self.assertTrue(value['installed']);self.assertFalse(value['helper_disposed'])
        self.assertIsNone(value['game_exit'])

    def test_unavailable_game_exit_code_is_explicit_without_false_success(self):
        value=self.run_case('exit_code_unavailable',capture=True)
        self.assertIsNone(value['failure']);self.assertTrue(value['disposed']);self.assertFalse(value['installed'])
        self.assertEqual(value['game_exit']['status'],'exit_code_unavailable');self.assertFalse(value['game_exit']['available']);self.assertIsNone(value['game_exit']['exit_code'])

    def test_game_exit_receipt_io_failure_cannot_interrupt_cleanup(self):
        value=self.run_case('game_exit_receipt_error',capture=True)
        self.assertIsNone(value['failure']);self.assertIsNone(value['game_exit']);self.assertEqual(value['killed'],[9222,8111]);self.assertTrue(value['disposed']);self.assertTrue(value['helper_disposed']);self.assertFalse(value['installed'])

    def test_before_adoption_failure_survives_locked_log_cleanup(self):
        value=self.run_case('capture_failure_locked_log',capture=True)
        self.assertIn('unqualified',value['failure']);self.assertEqual(value['launch_failure']['message'],value['failure']);self.assertFalse(value['launch_failure']['owned_game_adopted'])
        self.assertIsNone(value['game_exit']);self.assertEqual(value['killed'],[8111]);self.assertFalse(value['disposed']);self.assertTrue(value['helper_disposed']);self.assertTrue(value['installed'])

    def test_capture_receipt_io_failure_cannot_skip_retained_helper_or_file_cleanup(self):
        value=self.run_case('capture_receipt_error',capture=True);self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[9222,8111]);self.assertTrue(value['helper_disposed']);self.assertTrue(value['disposed']);self.assertFalse(value['installed'])

    def test_capture_helper_cleanup_failure_keeps_temporary_files(self):
        value=self.run_case('capture_helper_failure',capture=True);self.assertIsNone(value['failure']);self.assertEqual(value['killed'],[9222,8111]);self.assertFalse(value['helper_disposed']);self.assertTrue(value['installed'])

    def test_capture_start_failure_cleans_deployment_and_restores_environment(self):
        value=self.run_case('failure',capture=True);self.assertEqual(value['failure'],'fixture start failure');self.assertEqual(value['killed'],[]);self.assertFalse(value['installed']);self.assertEqual(value['app'],'original-app');self.assertIsNone(value['game'])

    def test_video_override_is_live_for_the_launch_and_restored_byte_for_byte(self):
        for case in ('deadline','stopped','failure'):
            with self.subTest(case=case):
                value=self.run_case(case,video=True)
                if case=='failure':self.assertEqual(value['failure'],'fixture start failure')
                else:self.assertIsNone(value['failure'])
                video=value['launch']['video']
                self.assertEqual(video['displayModeRef']['uid'],'WINDOWED');self.assertEqual(video['resolutionWidthRef']['id'],2560)
                self.assertEqual(video['resolutionHeightRef']['id'],1440);self.assertEqual(video['renderResolutionPresetRef']['uid'],'PERFORMANCE')
                self.assertEqual(value['video_json'],VIDEO_JSON);self.assertFalse(value['video_pending']);self.assertFalse(value['installed'])

    def test_plain_launch_never_touches_video_settings(self):
        value=self.run_case('deadline');self.assertIsNone(value['failure']);self.assertIsNone(value['launch']['video']['displayModeRef'])
        self.assertEqual(value['launch']['video']['renderResolutionPresetRef']['uid'],'ULTRA_PERFORMANCE');self.assertEqual(value['video_json'],VIDEO_JSON);self.assertFalse(value['video_pending'])

    def test_video_override_stays_pending_while_a_game_cannot_be_confirmed_closed(self):
        value=self.run_case('foreign',video=True);self.assertIsNone(value['failure']);self.assertTrue(value['installed'])
        self.assertTrue(value['video_pending'],'next launches must refuse until restore-video-settings.ps1');self.assertNotEqual(value['video_json'],VIDEO_JSON)

    def test_unrestored_override_refuses_every_launch_before_deployment(self):
        for video in (False,True):
            with self.subTest(video=video):
                value=self.run_case('deadline',video=video,pending=True);self.assertIn('restore-video-settings.ps1',value['failure'])
                self.assertFalse(value['started']);self.assertFalse(value['installed']);self.assertEqual(value['video_json'],VIDEO_JSON)

    def test_video_override_refuses_no_close_before_anything_starts(self):
        value=self.run_case('leave',video=True);self.assertIn('-NoClose cannot be combined',value['failure']);self.assertFalse(value['started']);self.assertFalse(value['installed']);self.assertEqual(value['video_json'],VIDEO_JSON)

    def test_output_size_launch_protects_settings_without_editing_them(self):
        value=self.run_case('deadline',video='-OutputSize 1944x2160')
        self.assertIsNone(value['failure']);self.assertEqual(value['launch']['output_size'],'1944x2160','the mod receives the size')
        self.assertEqual(value['launch']['video']['renderResolutionPresetRef']['uid'],'ULTRA_PERFORMANCE','JSON not edited')
        self.assertIsNone(value['launch']['video']['displayModeRef'])
        self.assertEqual(value['video_json'],VIDEO_JSON);self.assertFalse(value['video_pending']);self.assertFalse(value['installed'])

if __name__=='__main__':unittest.main()
