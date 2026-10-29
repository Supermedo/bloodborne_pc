# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'launcher'),str(ROOT/'scripts')]
import bbport_launcher_win as launcher
import bbport_fsr412
import bbport_lang

class DriverProviderTests(unittest.TestCase):
    def test_driver_vulkan_is_independent_of_fsr4vk(self):
        caps={'vendor_id':0x1002,'vk':{'supported':False,'reason':'hardware'},
              'fsr412_vulkan':dict(supported=True,reason='ready',detail='4.1.2'),
              'fsr412_dx12':dict(supported=True,reason='ready',detail='4.1.2'),
              'dx12_interop':dict(supported=True,reason='ready',detail='')}
        paths={'vulkan':'installed/amdvkffx64.dll','dx12':'installed/amdxcffx64.dll'}
        for method in ('vk','dx12'):
            self.assertTrue(bbport_fsr412.method_status(method,caps,paths)['supported'])
        caps.pop('fsr412_vulkan')
        self.assertEqual(bbport_fsr412.method_status('vk',caps,{})['reason'],'driver')

    def test_selected_adapter_and_pending_checks(self):
        self.assertEqual(bbport_fsr412.method_status('vk',None,{})['reason'],'checking')
        self.assertEqual(bbport_fsr412.method_status('vk',{'vendor_id':0x10de},{'vulkan':'present'})['reason'],'hardware')

    def test_all_saved_provider_combinations(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'bbport.ini'
            with patch.object(launcher,'ini_path',return_value=path), \
                 patch('bbport_driver_runtime.discover',return_value={}):
                for version in ('fsr411','fsr412'):
                    for backend in ('vk','dx12'):
                        for fg in ('off','fsr3','fsr401','fsr402'):
                            values={**launcher.INI_DEFAULTS,'upscaler':version,'fsr411_backend':backend,'frame_gen':fg}
                            launcher.save_ini(values,[])
                            loaded,_=launcher.load_ini()
                            self.assertEqual((loaded['upscaler'],loaded['fsr411_backend'],loaded['frame_gen']),
                                             (version,backend,fg))
                            env=launcher.game_environment(launcher.APP_DEFAULTS,loaded)
                            self.assertEqual(env['BB_FSR411_BACKEND'],backend)
                            self.assertEqual(env['BB_FSR411_NATIVE'],'1' if backend=='dx12' else '0')

    def test_regular_profile_and_explicit_paths_are_preserved(self):
        with patch.dict(os.environ,{'BB_CONFIG':'old.ini','BB_DATA_DIR':'old-data',
              'BB_USER_DIR':'old-saves','BB_GPU_USER_DIR':'old-cache','BB_TEST_AMD_VK_RUNTIME':'old.dll'}), \
             patch('bbport_driver_runtime.discover',return_value={}):
            env=launcher.game_environment(launcher.APP_DEFAULTS,launcher.INI_DEFAULTS)
        self.assertEqual(env['BB_CONFIG'],'old.ini')
        self.assertEqual(env['BB_DATA_DIR'],'old-data')
        self.assertEqual(env['BB_USER_DIR'],'old-saves')
        self.assertEqual(env['BB_GPU_USER_DIR'],'old-cache')
        self.assertNotIn('BB_TEST_AMD_VK_RUNTIME',env)
        self.assertEqual(launcher.CONFIG_FILE,Path(os.environ.get('APPDATA',Path.home()))/'bbport-launcher/settings.json')
        self.assertTrue(launcher.APP_DEFAULTS['check_updates'])
        with patch.dict(os.environ,{'BB_CONFIG':'chosen.ini'}):
            self.assertEqual(launcher.ini_path(),Path('chosen.ini'))

    def test_labels_in_every_launcher_language(self):
        choices=dict(launcher.UPSCALERS)
        self.assertEqual(choices['fsr411'][0],'FSR 4.1.1 (experimental)')
        self.assertEqual(choices['fsr412'][0],'FSR 4.1.2 (experimental)')
        old=launcher.LANG
        try:
            for lang,_ in bbport_lang.LANGUAGE_NAMES:
                if not lang: continue
                launcher.LANG=lang
                self.assertIn('4.1.2',launcher._(*choices['fsr412']))
                self.assertNotIn('prototype',launcher._(*choices['fsr412']))
                self.assertIn('4.0.1',launcher._(*dict(launcher.FRAME_GENERATION)['fsr401']))
                self.assertIn('4.0.2',launcher._(*dict(launcher.FRAME_GENERATION)['fsr402']))
        finally: launcher.LANG=old

if __name__=='__main__': unittest.main()
