import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock
import json
import tempfile
from types import SimpleNamespace

scripts=Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0,str(scripts))
spec=importlib.util.spec_from_file_location('capture_hand_visibility',scripts/'capture_hand_visibility.py')
capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)


class HandCaptureTests(unittest.TestCase):
    def test_only_requests_current_script_at_settled_headings(self):
        for index in range(8):
            status=dict(id='current',state='running',elapsed_ms=index*14000+3500)
            self.assertEqual(capture.capture_name(index,status,'current'),'heading_'+str(index*45))
            for change in (dict(id='old'),dict(state='clock_gap'),dict(state='completed'),
                           dict(elapsed_ms=index*14000+3499),dict(elapsed_ms=index*14000+8000)):
                self.assertIsNone(capture.capture_name(index,dict(status,**change),'current'))
        self.assertIsNone(capture.capture_name(8,dict(id='current',state='running',elapsed_ms=119000),'current'))

    def run_joined(self, fault=None):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);mailbox=root/'driver';output=root/'output';mailbox.mkdir();output.mkdir()
            status=mailbox/'status.json';status.write_text(json.dumps(dict(id='hands_test',state='running',elapsed_ms=3500)))
            now=[0.0]
            def tick(_):
                now[0]+=.1
                self.assertEqual((output/'joined_capture_request.txt').read_text(),'joined_case\n')
                self.assertFalse((output/'request.txt').exists());self.assertFalse((output/'draw_visibility_request.txt').exists())
                folder=output/'joined_case';folder.mkdir(exist_ok=True)
                for eye in ('left.ppm','right.ppm'):(folder/eye).write_bytes(b'pixels')
                pair=dict(dispatch=100,submitted=True,eyes=[dict(eye=i,render_id=99) for i in range(2)],joined_capture=dict(id='joined_case',draw_dispatch=100,draw_render_id=99,verified=True))
                draw=dict(model='ordinary_object_visibility_v1',dispatch=100,render_id=99,joined_capture_requested=True,eyes=[dict(complete=True,invalid=0,truncated=0) for _ in range(2)])
                if fault:fault(pair,draw)
                (folder/'pair.json').write_text(json.dumps(pair));(output/'joined_case.draw_visibility.json').write_text(json.dumps(draw))
                status.write_text(json.dumps(dict(id='hands_test',state='completed',elapsed_ms=8000)))
            with mock.patch.object(capture.uuid,'uuid4',return_value=SimpleNamespace(hex='test')),mock.patch.object(capture.driver,'write_request'),mock.patch.object(capture.time,'monotonic',side_effect=lambda:now[0]),mock.patch.object(capture.time,'sleep',side_effect=tick):
                ok=capture.run(mailbox,output,names=('joined_case',),draw_visibility=True,joined_capture=True)
            return ok,json.loads((output/'sweep_requests.json').read_text())

    def test_one_native_request_requires_exact_image_draw_frame(self):
        ok,receipt=self.run_joined();self.assertTrue(ok);self.assertTrue(receipt['captures'][0]['exact_image_draw_join'])

    def test_files_alone_never_qualify_a_mismatched_or_legacy_frame(self):
        for fault in (lambda p,d:p.update(dispatch=101),lambda p,d:p['eyes'][1].update(render_id=98),
                      lambda p,d:p['joined_capture'].update(verified=False),lambda p,d:d.update(joined_capture_requested=False),
                      lambda p,d:p['joined_capture'].update(id='other')):
            with self.subTest(fault=fault):
                ok,receipt=self.run_joined(fault);self.assertFalse(ok);self.assertFalse(receipt['captures'][0]['exact_image_draw_join'])
