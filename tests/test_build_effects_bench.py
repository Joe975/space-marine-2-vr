import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('effects_bench', Path(__file__).resolve().parents[1] / 'scripts/build_effects_bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

HDR = ('preset   =   {\n   Tonemapping   =   {\n      WhitePoint   =   10\n      Intensity   =   7\n   }\n'
       '   Bloom   =   {\n      BloomPoint   =   0.89\n      Intensity   =   1\n   }\n'
       '   MayaExposure   =   1\n   __type__   =   "hdr"\n}\n')
SKY = ('preset   =   {\n   sunFlare   =   {\n      flareColor   =   [\n         255,\n         200\n      ]\n'
       '      flareIntensity   =   70\n      flareSize   =   15\n   }\n   sunDisc   =   {\n      discIntensity   =   36\n   }\n'
       '   __type__   =   "new_sky"\n}\n')
GRAIN = 'preset   =   {\n   enabled   =   True\n   tiling   =   {\n      enabled   =   True\n   }\n   __type__   =   "film_grain_new"\n}\n'


class PatchTests(unittest.TestCase):
    def test_bloom_only_touches_the_bloom_block(self):
        out, changed = bench.patch(HDR, bench.GROUPS['bloom']['hdr'])
        self.assertEqual(changed, 2)
        self.assertIn('      Intensity   =   0\n      Enabled   =   False\n   }\n   MayaExposure', out)
        self.assertIn('WhitePoint   =   10\n      Intensity   =   7', out)  # tonemapping untouched
        self.assertEqual(bench.patch(out, bench.GROUPS['bloom']['hdr']), (out, 0))  # idempotent

    def test_existing_enabled_is_switched_not_duplicated(self):
        text = HDR.replace('Intensity   =   1\n', 'Intensity   =   1\n      Enabled   =   True\n')
        out, changed = bench.patch(text, bench.GROUPS['bloom']['hdr'])
        self.assertEqual(changed, 2)
        self.assertEqual(out.count('Enabled'), 1)
        self.assertIn('Enabled   =   False', out)

    def test_sun_flare_keeps_disc_and_arrays(self):
        out, changed = bench.patch(SKY, bench.GROUPS['sun_flare']['new_sky'])
        self.assertEqual(changed, 1)
        self.assertIn('flareIntensity   =   0', out)
        self.assertIn('discIntensity   =   36', out)
        self.assertIn('         255,\n         200\n', out)

    def test_top_level_field_ignores_nested_namesake(self):
        out, changed = bench.patch(GRAIN, bench.GROUPS['camera_fx']['film_grain_new'])
        self.assertEqual(changed, 1)
        self.assertIn('   enabled   =   False\n   tiling', out)
        self.assertIn('      enabled   =   True\n', out)

    def test_rejects_unbalanced_and_unknown(self):
        with self.assertRaises(ValueError):
            bench.patch(HDR.rstrip('}\n'), bench.GROUPS['bloom']['hdr'])
        with self.assertRaises(ValueError):
            bench.groups_of('bloom+fog')
        self.assertEqual(bench.groups_of('all'), sorted(bench.GROUPS))


class BuildTests(unittest.TestCase):
    def test_pack_keeps_base_entries_and_edits_matching_presets(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            base, presets = tmp / 'base.pak', tmp / 'presets.pak'
            with zipfile.ZipFile(base, 'w') as z:
                z.writestr('ssl/camera/x.sso', b'camera')
            with zipfile.ZipFile(presets, 'w') as z:
                z.writestr('presets/data/a/hdr.s3dprs', HDR)
                z.writestr('presets/data/a/sky.s3dprs', SKY)
            manifest = bench.build(base, presets, tmp / 'out', 'bloom')
            self.assertEqual(manifest['edited_presets_by_type'], {'hdr': 1})
            pak = tmp / 'out' / 'bloom' / bench.PAK_NAME
            with zipfile.ZipFile(pak) as z:
                self.assertEqual(sorted(z.namelist()), ['presets/data/a/hdr.s3dprs', 'ssl/camera/x.sso'])
                self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))
                self.assertIn(b'Enabled   =   False', z.read('presets/data/a/hdr.s3dprs'))
            self.assertEqual(json.loads(pak.with_suffix('.json').read_text())['variant'], 'bloom')

    def test_preset_in_base_pack_is_edited_from_the_base_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            base, presets = tmp / 'base.pak', tmp / 'presets.pak'
            grain_off = GRAIN.replace('   enabled   =   True\n   tiling', '   enabled   =   False\n   tiling')
            with zipfile.ZipFile(base, 'w') as z:
                z.writestr('presets/data/a/hdr.s3dprs', HDR.replace('BloomPoint   =   0.89', 'BloomPoint   =   2'))
                z.writestr('presets/data/a/grain.s3dprs', grain_off)
            with zipfile.ZipFile(presets, 'w') as z:
                z.writestr('presets/data/a/hdr.s3dprs', HDR)
                z.writestr('presets/data/a/grain.s3dprs', GRAIN)
            bench.build(base, presets, tmp / 'out', 'bloom+camera_fx')
            with zipfile.ZipFile(tmp / 'out' / 'bloom+camera_fx' / bench.PAK_NAME) as z:
                hdr = z.read('presets/data/a/hdr.s3dprs').decode()
                self.assertIn('BloomPoint   =   2', hdr)  # the base pack's version, then bloom off
                self.assertIn('Enabled   =   False', hdr)
                self.assertEqual(z.read('presets/data/a/grain.s3dprs').decode(), grain_off)  # already off: kept


if __name__ == '__main__':
    unittest.main()
