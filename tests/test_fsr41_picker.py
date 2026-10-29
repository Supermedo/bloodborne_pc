# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'launcher'),str(ROOT/'scripts')]
import bbport_fsr41 as model
import bbport_fsr41_ui as ui
import bbport_fsr412


class PickerTests(unittest.TestCase):
    def test_status_diagnosis_distinguishes_proof_from_missing_features(self):
        cases=[('hardware','AMD GPU required',model.GPU),
               ('hardware','FSR 4.1.2 unsupported by selected GPU',model.GPU),
               ('hardware','VK_EXT_descriptor_buffer',model.CAPABILITY),
               ('provider','FSR provider unavailable',model.CAPABILITY),
               ('driver','provider version unavailable',model.DRIVER),
               ('download','changed file',model.FILES),('files','missing.dll',model.FILES),
               ('interop','fence',model.INTEROP),('check','',model.CHECK),('checking','',model.CHECKING)]
        for reason,detail,expected in cases:
            self.assertEqual(model.code(dict(supported=False,reason=reason,detail=detail)),expected)
        self.assertEqual(model.code(dict(supported=True,reason='ready')),model.SUPPORTED)

    def test_no_unproven_driver_age_claim_and_complete_translations(self):
        self.assertEqual(len(model.TEXTS),13)
        for language,texts in model.TEXTS.items():
            self.assertEqual(len(texts),len(model.KEYS),language)
            self.assertTrue(all(texts),language)
        self.assertNotIn('Driver too old',model.KEYS)
        self.assertEqual(len({row[:2] for row in model.ROUTES}),4)

    def test_selecting_unavailable_route_preserves_both_preferences(self):
        class Variable:
            def __init__(self,value):self.value=value
            def set(self,value):self.value=value
            def get(self):return self.value
        class App:
            def __init__(self):self.values={'upscaler':Variable('fsr411'),'fsr411_backend':Variable('dx12'),'fsr41_version':Variable('fsr411')}
            def var(self,key,store):return self.values[key]
        app=App()
        with patch.object(ui,'statuses',return_value=[dict(supported=False)]*4):
            self.assertFalse(ui.select(app,0))
        self.assertEqual((app.var('upscaler','ini').get(),app.var('fsr411_backend','ini').get()),('fsr411','dx12'))
        with patch.object(ui,'statuses',return_value=[dict(supported=True)]*4):
            for index,row in enumerate(model.ROUTES):
                self.assertTrue(ui.select(app,index))
                self.assertEqual((app.var('upscaler','ini').get(),app.var('fsr411_backend','ini').get()),row[:2])

    def test_dx12_provider_needs_exact_driver_and_bridge_support(self):
        caps={'vendor_id':0x1002,'dx12_interop':dict(supported=True,reason='ready',detail=''),
              'fsr412_dx12':dict(supported=False,reason='driver',detail='provider unavailable')}
        self.assertFalse(bbport_fsr412.method_status('dx12',caps)['supported'])
        caps['fsr412_dx12']=dict(supported=True,reason='ready',detail='4.1.2')
        self.assertTrue(bbport_fsr412.method_status('dx12',caps)['supported'])
        caps['dx12_interop']=dict(supported=False,reason='interop',detail='shared fence')
        self.assertEqual(bbport_fsr412.method_status('dx12',caps)['reason'],'interop')

    def test_inactive_family_preference_roundtrips(self):
        import tempfile
        import bbport_launcher_win as launcher
        with tempfile.TemporaryDirectory() as temp, patch.object(launcher,'ini_path',return_value=Path(temp)/'bbport.ini'):
            for version in ('fsr411','fsr412'):
                for backend in ('vk','dx12'):
                    settings={**launcher.INI_DEFAULTS,'upscaler':'fsr3','fsr41_version':version,'fsr411_backend':backend}
                    launcher.save_ini(settings,[])
                    read,_=launcher.load_ini()
                    self.assertEqual((read['upscaler'],read['fsr41_version'],read['fsr411_backend']),('fsr3',version,backend))


if __name__=='__main__':unittest.main()
