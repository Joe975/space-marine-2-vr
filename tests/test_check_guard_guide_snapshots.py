from pathlib import Path
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import check_guard_guide_snapshots as guide  # noqa: E402


def snapshot(root, name, states, pixel=(20, 30), colour=(80, 220, 110)):
    folder = Path(root) / name
    folder.mkdir()
    width, height = 64, 48
    rgb = bytearray([100] * (width * height * 3))
    for y in range(pixel[1] - 3, pixel[1] + 4):
        for x in range(pixel[0] - 3, pixel[0] + 4):
            rgb[(y * width + x) * 3:(y * width + x) * 3 + 3] = bytes(colour)
    for eye in ('left', 'right'):
        (folder / f'{eye}.ppm').write_bytes(b'P6\n64 48\n255\n' + bytes(rgb))
    eyes = []
    for index, state in enumerate(states):
        eye = {'eye': index, 'guard_guide': state}
        if state != 'hidden':
            eye['guard_guide_pixel'] = list(pixel)
        eyes.append(eye)
    (folder / 'pair.json').write_text(json.dumps({'eyes': eyes}))


class GuardGuideSnapshotTests(unittest.TestCase):
    def test_states_must_match_in_both_eyes(self):
        with tempfile.TemporaryDirectory() as root:
            snapshot(root, 'active', ['active', 'active'])
            snapshot(root, 'none', ['hidden', 'hidden'])
            snapshot(root, 'split', ['active', 'idle'])
            ok, rows = guide.check(root, [('active', 'active'), ('none', 'hidden')])
            self.assertTrue(ok)
            self.assertEqual(rows[0]['colour'], [80, 220, 110], 'colour is read at the recorded grip pixel')
            ok, rows = guide.check(root, [('split', 'active')])
            self.assertFalse(ok, 'one eye without the expected state fails')
            ok, rows = guide.check(root, [('none', 'idle')])
            self.assertFalse(ok, 'a missing guide fails an expected visible state')
            ok, rows = guide.check(root, [('missing', 'idle')])
            self.assertFalse(ok)
            self.assertIn('no snapshot', rows[0]['error'])

    def test_cli_and_sheet(self):
        with tempfile.TemporaryDirectory() as root:
            snapshot(root, 'idle', ['idle', 'idle'], colour=(210, 220, 230))
            snapshot(root, 'none', ['hidden', 'hidden'])
            sheet = Path(root) / 'sheet.png'
            self.assertEqual(guide.main(['x', root, 'idle=idle', 'none=hidden', '--sheet', str(sheet)]), 0)
            self.assertTrue(sheet.exists())
            self.assertEqual(guide.main(['x', root, 'idle=active']), 1)
            with self.assertRaises(SystemExit):
                guide.main(['x', root, 'idle=purple'])


if __name__ == '__main__':
    unittest.main()
