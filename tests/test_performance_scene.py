import io
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from analyze_performance_scene import analyze


class CameraEvidence(unittest.TestCase):
    def fixture(self, moved=False, rotated=False):
        lines=[]
        for t in (2,7,12,17,22,27):
            x = 2 if moved and t==17 else 1
            row = '0,1,0' if rotated and t==17 else '1,0,0'
            lines.extend((f'[{13:02}:00:{t:02}.000] [I] NativeCameraSource: caller=0x18033bf source=a center=[{x},2,3] age_ms=7',
                          f'[{13:02}:00:{t:02}.000] [I] EngineCameraProbe: caller=0x18033bf calls=20 row0=[{row}] row1=[0,1,0] row2=[0,0,1]'))
        return io.StringIO('\n'.join(lines))

    def report(self, log):
        return analyze(log,{'qpc_start_ms':1000,'qpc_end_ms':31000},{'utc':'2026-10-15T00:00:00Z','qpc_ms':1000})

    def test_stable_and_moving_native_camera_are_distinguished(self):
        self.assertTrue(self.report(self.fixture())['sampled_camera_stable'])
        moved=self.report(self.fixture(moved=True))
        self.assertFalse(moved['sampled_camera_stable'])
        self.assertEqual(moved['max_displacement_game_units'],1)
        self.assertFalse(self.report(self.fixture(rotated=True))['sampled_camera_stable'])

    def test_stale_missing_and_sparse_observations_cannot_prove_stability(self):
        for text in ('', self.fixture().getvalue().replace('age_ms=7','age_ms=1000')):
            with self.assertRaises(ValueError): self.report(io.StringIO(text))
        sparse='\n'.join(line for line in self.fixture().getvalue().splitlines() if not any(f':{t:02}.000' in line for t in (7,12,17)))
        self.assertFalse(self.report(io.StringIO(sparse))['sampled_camera_stable'])


if __name__ == '__main__': unittest.main()
