"""Configure the actual checked source transformation without building/running graphics."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path('C:/Repos/sm2-vr/refs/vrframework-core/src/D3D12Hook.cpp')


class FrameworkCaptureSourceTests(unittest.TestCase):
    def generate(self, source, directory):
        script = directory / 'generate.cmake'
        script.write_text(f'include("{ROOT.as_posix()}/cmake/FrameworkCaptureCompatibility.cmake")\n'
                          f'sm2_framework_capture_source("{source.as_posix()}" "{directory.as_posix()}/output")\n')
        return subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True, timeout=10)

    def test_only_initial_inspection_and_include_change(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            result = self.generate(SOURCE, directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            original = SOURCE.read_text()
            needle = 'const auto original_bytes = utility::get_original_bytes(d3d12_create_device);'
            replacement = 'const auto original_bytes = sm2_capture_compat::dummy_original_bytes(reinterpret_cast<uintptr_t>(d3d12_create_device), [&] { return utility::get_original_bytes(d3d12_create_device); }, [](uintptr_t target, bool retained, const char* module) { spdlog::info("FrameworkCaptureCompat: dummy target={:x} module={} retain_hook={}", target, module, retained); });'
            actual = (directory / 'output/D3D12Hook.cpp').read_text()
            self.assertEqual(actual, '#include "FrameworkCaptureCompatibility.h"\n' + original.replace(needle, replacement))
            block = (directory / 'output/framework_capture_dummy.inc').read_text()
            self.assertEqual(block.count(replacement), 1)
            self.assertIn('memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size())', block)
            self.assertIn('d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device))', block)

    def test_source_drift_refuses_before_publication(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            directory = Path(tmp)
            changed = directory / 'D3D12Hook.cpp'
            changed.write_bytes(SOURCE.read_bytes() + b'\n// drift\n')
            result = self.generate(changed, directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires source review', ' '.join(result.stderr.split()))
            self.assertFalse((directory / 'output').exists())


if __name__ == '__main__':
    unittest.main()
