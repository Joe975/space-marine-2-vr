import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stillness", ROOT / "scripts/check_execution_stillness.py")
stillness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stillness)


def trace(hand_speed, seconds, hz=30, units=0.9):
    """Samples whose right hand moves at hand_speed(t) m/s; the other points stay put."""
    out, x = [], 0.0
    for i in range(int(seconds * hz) + 1):
        t = i / hz
        if i:
            x += hand_speed(t) * units / hz
        out.append((t, [[0, 0, 0], [0, 2, 0], [0, 1, 0], [x, 1, 0]]))
    return out


class StillnessRuleTests(unittest.TestCase):
    def test_speeds_span_the_interval_and_skip_gaps(self):
        rates = stillness.speeds(trace(lambda t: 2.0, 1), 0.9)
        self.assertTrue(rates and all(abs(v - 2.0) < 1e-6 for _, v in rates))
        self.assertTrue(all(b - a >= stillness.INTERVAL - 1e-9 for (a, _), (b, _) in zip(rates, rates[1:])))
        gap = [(0.0, [[0, 0, 0]]), (0.5, [[5, 0, 0]]), (0.6, [[5, 0, 0]])]
        self.assertEqual(stillness.speeds(gap, 1), [(0.6, 0.0)])

    def test_rule_holds_half_a_second_after_the_marine_stops(self):
        rates = stillness.speeds(trace(lambda t: 3.0 if t < 4.0 else 0.02, 6), 0.9)
        rule = stillness.rule_time(rates, 0.8, 0.35, 0.5)
        self.assertGreaterEqual(rule, 4.5)
        self.assertLess(rule, 4.6)

    def test_pauses_shorter_than_the_still_time_never_end_it(self):
        rates = stillness.speeds(trace(lambda t: 0.1 if (t % 1.0) < 0.4 else 2.0, 5), 0.9)
        self.assertIsNone(stillness.rule_time(rates, 0.8, 0.35, 0.5))


if __name__ == "__main__":
    unittest.main()
