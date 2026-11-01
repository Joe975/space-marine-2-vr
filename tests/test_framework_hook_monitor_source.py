"""Configure the hook monitor source transformation (cmake/FrameworkHookMonitor.cmake) without building."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REFS = ROOT / 'refs/vrframework-core/src'
if not REFS.exists():
    REFS = Path('C:/Repos/sm2-vr/refs/vrframework-core/src')
FRAMEWORK = REFS / 'Framework.cpp'
D3D12 = REFS / 'D3D12Hook.cpp'

GUARD_LINES = [
    '            if (!m_has_last_chance && now - m_last_chance_time > std::chrono::seconds(1) && !m_is_d3d11 && d3d12 != nullptr && sm2_framework_monitor::present_hook_intact(d3d12->is_hooked())) {',
    '                spdlog::info("D3D12 present hook is still intact, ignoring...");',
    '                m_last_present_time = now;',
    '                m_last_chance_time = now;',
    '                m_has_last_chance = true;',
    '            }',
]
REHOOK = '            if (!m_has_last_chance && now - m_last_chance_time > std::chrono::seconds(1)) {'
PRESENT_HOOK = '        m_present_hook = std::make_unique<PointerHook>(&present_fn, (void*)&D3D12Hook::present);'
REMEMBER = '        sm2_framework_monitor::remember(&present_fn, (void*)&D3D12Hook::present);'


@unittest.skipUnless(FRAMEWORK.exists(), 'framework refs not fetched (scripts/fetch-tools.sh)')
class FrameworkHookMonitorSourceTests(unittest.TestCase):
    def run_cmake(self, directory, body):
        script = directory / 'generate.cmake'
        script.write_text(f'include("{ROOT.as_posix()}/cmake/FrameworkHookMonitor.cmake")\n{body}\n')
        return subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True, timeout=10)

    def test_framework_gets_only_the_guard_before_the_rehook(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            result = self.run_cmake(directory, f'sm2_framework_monitor_source("{FRAMEWORK.as_posix()}" "{directory.as_posix()}/output")')
            self.assertEqual(result.returncode, 0, result.stderr)
            original = FRAMEWORK.read_bytes().decode()
            self.assertIn('\r\n', original)
            guard = '\r\n'.join(GUARD_LINES) + '\r\n'
            expected = '#include "FrameworkHookMonitor.h"\r\n' + original.replace(REHOOK, guard + REHOOK)
            actual = (directory / 'output/Framework.cpp').read_bytes().decode()
            self.assertEqual(actual, expected)
            self.assertEqual(actual.count('Sending rehook request for D3D'), 1)
            block = (directory / 'output/framework_monitor_stall.inc').read_bytes().decode()
            self.assertTrue(block.startswith('        // check if present time is more than 5 seconds ago'))
            self.assertIn(guard + REHOOK, block)
            self.assertIn('hook_d3d12();', block)
            self.assertNotIn('m_last_message_time > std::chrono::seconds(5)', block)

    def test_d3d12_hook_records_the_present_slot_once(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            generated = directory / 'D3D12Hook.cpp'
            generated.write_bytes(D3D12.read_bytes())
            result = self.run_cmake(directory, f'sm2_framework_monitor_d3d12("{generated.as_posix()}")')
            self.assertEqual(result.returncode, 0, result.stderr)
            original = D3D12.read_bytes().decode()
            expected = '#include "FrameworkHookMonitor.h"\r\n' + original.replace(PRESENT_HOOK, PRESENT_HOOK + '\r\n' + REMEMBER)
            self.assertEqual(generated.read_bytes().decode(), expected)

    def test_source_drift_refuses_before_publication(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            changed = directory / 'Framework.cpp'
            changed.write_bytes(FRAMEWORK.read_bytes() + b'\n// drift\n')
            result = self.run_cmake(directory, f'sm2_framework_monitor_source("{changed.as_posix()}" "{directory.as_posix()}/output")')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires source review', ' '.join(result.stderr.split()))
            self.assertFalse((directory / 'output').exists())

    def test_missing_present_hook_refuses(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            generated = directory / 'D3D12Hook.cpp'
            generated.write_bytes(D3D12.read_bytes().replace(PRESENT_HOOK.encode(), b'        // present hook moved'))
            result = self.run_cmake(directory, f'sm2_framework_monitor_d3d12("{generated.as_posix()}")')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires source review', ' '.join(result.stderr.split()))


if __name__ == '__main__':
    unittest.main()
