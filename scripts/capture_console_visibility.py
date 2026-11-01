"""Eight settled console views with hands lowered; requires a fresh snapshot launch.

This uses actual submitted eyes without opening SteamVR's mirror. Capture
completion is evidence of image provenance, never an automatic visual pass.
"""
import argparse
from pathlib import Path
from capture_hand_visibility import run
import vr_test_driver as driver

POSES = ((60, -10), (75, -10), (90, -10), (105, -10), (120, -10),
         (90, 0), (90, 10), (90, -20))
NAMES = tuple(f'console_yaw{yaw}_pitch{pitch}'.replace('-', 'minus') for yaw, pitch in POSES)
SCENARIO = driver.ROOT / 'config/test_driver/stereo_console_visibility.json'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--draw-visibility', action='store_true')
    parser.add_argument('--contrast', action='store_true', help='Only yaw90 pitch-10/+10 (two snapshots, 28 seconds)')
    args = parser.parse_args()
    scenario = driver.ROOT / 'config/test_driver/stereo_console_contrast.json' if args.contrast else SCENARIO
    names = (NAMES[2], NAMES[6]) if args.contrast else NAMES
    raise SystemExit(0 if run(driver.mailbox(args.directory), args.output, scenario, names, args.draw_visibility) else 1)
