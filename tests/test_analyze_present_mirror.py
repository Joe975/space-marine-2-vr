import importlib.util
from pathlib import Path
import unittest
from test_analyze_private_compositor import control as private_control, PIXELS

spec=importlib.util.spec_from_file_location('present_mirror',Path(__file__).resolve().parents[1]/'scripts/analyze_present_mirror.py')
audit=importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def control():
    c=private_control()
    c.update(model='ordinary_private_compositor_mirror_v1',desktop_mirror_verified=True)
    c['copy']['image_identity']=0x900000
    c['mirror']=dict(tag=c['copy']['tag'],producer_fence=1,fence_value=1,image=0x900000,
                     destination=c['redirect']['source'],queue=c['copy']['queue'],result=0,submitted=True,completed=True)
    return c


class PresentMirrorAuditTests(unittest.TestCase):
    def test_completed_exact_mirror_is_not_stereo_or_hud_parity(self):
        result=audit.analyze(control(),PIXELS)
        self.assertTrue(result['ordinary_present_mirror_pass'],result)
        for field in ('gpu_pair_verified','hud_parity_verified','histories_verified','colour_parity_verified','general_retirement_verified','runtime_submission'):
            self.assertIs(result[field],False)

    def test_every_mirror_identity_and_completion_is_required(self):
        changes=[(key,0) for key in ('tag','producer_fence','fence_value','image','destination','queue')]
        changes += [(key,7) for key in ('tag','producer_fence','image','destination','queue')]
        changes += [('submitted',False),('completed',False),('result',-1),('result',True),('image',True)]
        for key,value in changes:
            c=control();c['mirror'][key]=value
            with self.subTest(key=key,value=value):self.assertFalse(audit.analyze(c,PIXELS)['ordinary_present_mirror_pass'])
        for key in control()['mirror']:
            c=control();del c['mirror'][key]
            with self.subTest(missing=key):self.assertFalse(audit.analyze(c,PIXELS)['ordinary_present_mirror_pass'])

    def test_cannot_borrow_source_identity_or_incomplete_producer(self):
        for key,value in [('image_identity',0),('completed',False),('submitted',False),('identity',0),('fence_value',2)]:
            c=control();c['copy'][key]=value
            self.assertFalse(audit.analyze(c,PIXELS)['ordinary_present_mirror_pass'])
        for key,value in [('mirror',None),('desktop_mirror_verified',False),('model','ordinary_private_compositor_v1'),('cpu_boundary',None)]:
            c=control();c[key]=value
            self.assertFalse(audit.analyze(c,PIXELS)['ordinary_present_mirror_pass'])


if __name__=='__main__':unittest.main()
