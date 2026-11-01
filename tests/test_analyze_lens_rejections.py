import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('lens_events', Path(__file__).resolve().parents[1] / 'scripts/analyze_lens_rejections.py')
lens = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lens)


class LensEventTests(unittest.TestCase):
    def test_success_after_refusal_cannot_pass_zero_refusals(self):
        text = ('PipLensRejection: serial=1 reason=2 phase=1 updates=513\n'
                'PipLensRejectionCount: reason=2 count=1 total=1\n'
                'PipFinalLensState: applied=4094 rejected=1 reason=0\n'
                'PipExperiment: finished=true lost=false updates=4096\n')
        result = lens.analyze(text)
        self.assertTrue(result['evidence_complete'])
        self.assertFalse(result['zero_refusals_pass'])
        self.assertEqual(result['reason_counts'], {2: 1})
        for changed in (text.replace('serial=1', 'serial=2'), text.replace('count=1', 'count=2'),
                        text.replace('reason=2 phase', 'reason=99 phase'), text + 'PipLensRejectionOverflow: missing=1',
                        text.replace('finished=true', 'finished=false'), text.replace('lost=false', 'lost=true'),
                        text.replace('updates=4096', 'updates=100'), text.replace('rejected=1', 'rejected=oops')):
            self.assertFalse(lens.analyze(changed)['evidence_complete'])

    def test_missing_events_cannot_hide_rejections_or_unknown_completion(self):
        text = ('PipFinalLensState: applied=4095 rejected=0 reason=0\n'
                'PipExperiment: finished=true lost=false updates=4096')
        self.assertTrue(lens.analyze(text)['zero_refusals_pass'])
        self.assertFalse(lens.analyze(text.replace('applied=4095', 'applied=0'))['zero_refusals_pass'])
        self.assertFalse(lens.analyze(text.replace('rejected=0', 'rejected=1'))['evidence_complete'])
        self.assertFalse(lens.analyze('')['zero_refusals_pass'])

    def test_strict_candidate_accounting_cannot_credit_skips_as_applications(self):
        clean = ('PipFinalLensState: applied=4095 rejected=0 reason=0\n'
                 'PipExperiment: finished=true lost=false updates=4096\n'
                 'PipLensAccounting: skipped=0 no_target=0 provider_error=0 foreign_record=0 unexpected_errors=0\n')
        self.assertTrue(lens.analyze(clean, require_accounting=True)['zero_refusals_pass'])
        self.assertFalse(lens.analyze(clean.split('PipLensAccounting:')[0], require_accounting=True)['evidence_complete'])
        skipped = clean.replace('skipped=0 no_target=0', 'skipped=1 no_target=1') + (
            'PipLensSkip: serial=1 reason=1 slot=1 record=0 active_ownership_verified=false\n')
        result = lens.analyze(skipped, require_accounting=True)
        self.assertTrue(result['evidence_complete'])
        self.assertFalse(result['zero_refusals_pass'])
        self.assertEqual(result['skipped_candidates'], 1)
        for changed in (skipped.replace('serial=1', 'serial=2'), skipped.replace('no_target=1', 'no_target=0'),
                        skipped.replace('active_ownership_verified=false', 'active_ownership_verified=true'),
                        skipped.replace('reason=1 slot', 'reason=4 slot'), skipped + 'PipLensSkipOverflow: missing=1'):
            self.assertFalse(lens.analyze(changed, require_accounting=True)['evidence_complete'])
        # Counting a skipped call does not repair the earlier missing-attempt gate.
        self.assertFalse(lens.analyze(skipped.replace('applied=4095', 'applied=4094'), require_accounting=True)['evidence_complete'])
        result = lens.analyze(clean.replace('unexpected_errors=0', 'unexpected_errors=1'), require_accounting=True)
        self.assertTrue(result['evidence_complete'])
        self.assertFalse(result['zero_refusals_pass'])

    def test_expired_owned_pose_is_an_explicit_refusal_not_an_unknown_reason(self):
        text = ('PipLensRejection: serial=1 reason=12 phase=1 updates=513\n'
                'PipLensRejectionCount: reason=12 count=1 total=1\n'
                'PipFinalLensState: applied=4094 rejected=1 reason=0\n'
                'PipExperiment: finished=true lost=false updates=4096\n')
        self.assertTrue(lens.analyze(text)['evidence_complete'])
        self.assertFalse(lens.analyze(text)['zero_refusals_pass'])


if __name__ == '__main__':
    unittest.main()
