import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"scripts"))
from analyze_query_dependencies import analyze


def row(name, t, value, duration=0):
    return dict(event="scene_left_dependency_"+name, qpc_start_ms=t, duration_ms=duration, value=value)


def fixture():
    return [row("summary", 20, 7), row("collects", 20, 1), row("reads", 20, 2),
            row("reads_before", 20, 1), row("matched_reads", 20, 1), row("gc_pending", 20, 3),
            row("query_count", 20, 2), row("keys_captured", 20, 2),
            row("publish_to_use_cpu", 10, 7, .25), row("consumer_rva", 10.25, 0x1e208be),
            row("consumer_result", 10.25, 0), row("consumer_column", 10.25, 1)]


class QueryDependencies(unittest.TestCase):
    def test_exact_scene_zero_result_is_real_visibility_dependency(self):
        result = analyze(reversed(fixture()), 0, 30)
        self.assertEqual(result["decision"], "no_go_for_simple_wait_deferral")
        self.assertEqual(result["eyes"]["left"]["first_consumer_rvas"], {"0x1e208be": 1})
        self.assertEqual(result["eyes"]["left"]["publish_to_consumer_ms"]["mean"], .25)

    def test_missing_metadata_or_other_window_cannot_claim_consumption(self):
        for rows, start in ((fixture()[:-1], 0), (fixture(), 11)):
            self.assertEqual(analyze(rows, start, 30)["decision"], "inconclusive")

    def test_missing_keys_never_prove_buffering_safe(self):
        rows = [r for r in fixture() if "consumer_" not in r["event"]]
        self.assertFalse(analyze(rows, 0, 30)["buffering_safety_proven"])

    def test_duplicates_and_impossible_owning_scene_fail(self):
        with self.assertRaises(ValueError):
            analyze(fixture()+[row("summary", 20, 8)], 0, 30)
        rows = fixture();rows[8] = row("publish_to_use_cpu", 21, 7, .25)
        with self.assertRaises(ValueError):
            analyze(rows, 0, 30)

    def test_unknown_result_is_not_proof(self):
        rows = fixture();rows[-2] = row("consumer_result", 10.25, -1)
        self.assertEqual(analyze(rows, 0, 30)["decision"], "inconclusive")

    def test_multiple_collections_are_ambiguous(self):
        rows = fixture();rows[1] = row("collects", 20, 2)
        self.assertEqual(analyze(rows, 0, 30)["decision"], "inconclusive")
