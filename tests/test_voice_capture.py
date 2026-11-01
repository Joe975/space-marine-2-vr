import importlib.util
from pathlib import Path
import unittest

import numpy as np

spec = importlib.util.spec_from_file_location("voice_capture", Path(__file__).resolve().parents[1] / "scripts" / "voice_capture.py")
voice = importlib.util.module_from_spec(spec)
spec.loader.exec_module(voice)


class VoiceCaptureTests(unittest.TestCase):
    def test_trigger_requires_whole_utterance(self):
        self.assertTrue(voice.is_trigger("Go ahead."))
        for text in ("Don't go ahead", "I'll go ahead and test the pistol", "go ahead now"):
            self.assertFalse(voice.is_trigger(text))
        self.assertFalse(voice.is_trigger("anything", ""))

    def test_device_never_falls_back_to_unrelated_microphone(self):
        devices = [{"name": "Valve output", "max_input_channels": 0},
                   {"name": "Desk mic", "max_input_channels": 1},
                   {"name": "Valve VR Radio & HMD Mic", "max_input_channels": 1}]
        self.assertEqual(voice.resolve_device(devices, "Valve")[0], 2)
        self.assertEqual(voice.resolve_device(devices, "1")[0], 1)
        with self.assertRaises(ValueError):
            voice.resolve_device(devices, "Missing")

    def test_pause_flush_preserves_capture_start_and_preroll(self):
        segmenter = voice.Segmenter(silence=0.09)
        quiet, speech = np.zeros(voice.BLOCK), np.ones(voice.BLOCK) * 0.1
        for i in range(8):
            self.assertIsNone(segmenter.feed(quiet, i))
        for i in range(5):
            self.assertIsNone(segmenter.feed(speech, 8 + i))
        self.assertIsNone(segmenter.feed(quiet, 13))
        self.assertIsNone(segmenter.feed(quiet, 14))
        stamp, audio = segmenter.feed(quiet, 15)
        self.assertEqual(stamp, 2)
        self.assertEqual(len(audio), 14 * voice.BLOCK)
        self.assertIsNone(segmenter.flush())

    def test_continuous_commentary_is_bounded_without_silence(self):
        segmenter = voice.Segmenter(maximum=0.3)
        speech = np.ones(voice.BLOCK) * 0.1
        chunks = [part for i in range(30) if (part := segmenter.feed(speech, i))]
        self.assertEqual([stamp for stamp, _ in chunks], [0, 10, 20])
        self.assertTrue(all(len(audio) == 10 * voice.BLOCK for _, audio in chunks))

    def test_silence_and_short_click_do_not_generate_transcripts(self):
        segmenter = voice.Segmenter(silence=0.09)
        quiet, click = np.zeros(voice.BLOCK), np.ones(voice.BLOCK) * 0.1
        self.assertIsNone(segmenter.feed(click, 0))
        for i in range(1, 20):
            self.assertIsNone(segmenter.feed(quiet, i))
        self.assertIsNone(segmenter.flush())


if __name__ == "__main__":
    unittest.main()
