# SPDX-License-Identifier: GPL-3.0-or-later
"""Release-boundary regressions: safe defaults, unavailable selections and direct startup."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'launcher'), str(ROOT/'scripts')]
import bbport_launcher_win as launcher
import bbport_fsr41 as family
import bbport_driver_runtime as runtime


class ReadinessTests(unittest.TestCase):
    def test_requested_hints_share_all_thirteen_translations(self):
        import bbport_lang_motion_cache as cache
        self.assertTrue(cache.KEYS[1].startswith('May solve general FSR FPS drops;'))
        self.assertEqual(family.HINTS[2],'Recommended if FSR 4.1.2 is unavailable or causes issues.')
        for language, texts in family.TEXTS.items():
            self.assertEqual(texts[-2:],cache.TEXTS[language])
            self.assertEqual(len(texts),len(family.KEYS))

    def test_automatic_recommendations_use_the_same_verified_route_order(self):
        import bbport_setup as setup
        ui=types.SimpleNamespace(rating=object(), app={}, display_size=lambda:(1920,1080),
            current_settings=lambda:{}, var=lambda *_:types.SimpleNamespace(get=lambda:'balanced'),
            fsr411_support=None)
        for mask in range(16):
            states=[dict(supported=bool(mask&(1<<i))) for i in range(4)]
            with patch.object(setup.hardware,'outputs',return_value={'1080p':'1920x1080'}), \
                 patch.object(setup.hardware,'recommended_class',return_value='1080p'), \
                 patch.object(setup.hardware,'profile',return_value=[('ini','upscaler','fsr3','quality')]), \
                 patch.object(family,'method_statuses',return_value=states):
                _res,_output,entries=setup.SetupMixin.recommended(ui)
            values={key:value for _store,key,value,_why in entries}
            selected=next((i for i in range(4) if states[i]['supported']),None)
            if selected is None:self.assertEqual(values['upscaler'],'fsr3')
            else:self.assertEqual((values['upscaler'],values['fsr411_backend']),family.ROUTES[selected][:2])

    def test_ranked_routes_and_remembered_selection_over_all_masks(self):
        for mask in range(16):
            states = [dict(supported=bool(mask & (1<<i))) for i in range(4)]
            for preferred in range(4):
                expected = next((i for i in (preferred,0,1,2,3) if states[i]['supported']),None)
                self.assertEqual(family.preferred_route(states, preferred), expected)
        for bad in (None, '', 1, 'true'):
            self.assertIsNone(family.preferred_route([dict(supported=bad)]*4))

    def test_partial_preferences_never_choose_an_unavailable_combination(self):
        with tempfile.TemporaryDirectory() as temp, \
             patch.object(launcher, 'ini_path', return_value=Path(temp)/'bbport.ini'), \
             patch.object(family, 'query', return_value={}), \
             patch.object(launcher.bbport_fsr411,'default_backend', return_value='dx12'):
            path = Path(temp)/'bbport.ini'
            states = [dict(supported=False), dict(supported=False),
                      dict(supported=True), dict(supported=False)]
            with patch.object(family,'method_statuses',return_value=states):
                path.write_text('fsr411_backend=dx12\n',encoding='utf-8')
                values,_=launcher.load_ini()
                self.assertEqual((values['upscaler'],values['fsr411_backend']),('fsr411','dx12'))
                path.write_text('upscaler=fsr411\n',encoding='utf-8')
                values,_=launcher.load_ini()
                self.assertEqual((values['upscaler'],values['fsr411_backend']),('fsr411','dx12'))
                path.write_text('upscaler=fsr412\nfsr411_backend=vk\n',encoding='utf-8')
                before=path.read_bytes()
                values,_=launcher.load_ini()
                self.assertEqual((values['upscaler'],values['fsr411_backend']),('fsr412','vk'))
                self.assertEqual(path.read_bytes(),before)

    def test_runtime_environment_shared_initialization_and_overrides(self):
        with patch.object(runtime,'discover',return_value={'vulkan':'new-vk.dll','dx12':'new-dx.dll'}):
            env={'BB_TEST_AMD_VK_RUNTIME':'explicit.dll','unrelated':'keep'}
            runtime.configure_environment(env)
            self.assertEqual(env['BB_TEST_AMD_VK_RUNTIME'],'explicit.dll')
            self.assertEqual(env['BB_TEST_AMD_DX_RUNTIME'],'new-dx.dll')
            runtime.configure_environment(env,replace=True)
            self.assertEqual(env['BB_TEST_AMD_VK_RUNTIME'],'new-vk.dll')
            self.assertEqual(env['unrelated'],'keep')
        with patch.object(runtime,'discover',return_value={}):
            runtime.configure_environment(env,replace=True)
        self.assertNotIn('BB_TEST_AMD_VK_RUNTIME',env)
        self.assertNotIn('BB_TEST_AMD_DX_RUNTIME',env)

    def test_direct_run_initializes_driver_environment_before_executable_lookup(self):
        spec=importlib.util.spec_from_file_location('run_readiness',ROOT/'run.py')
        runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
        cwd=Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as temp, patch.object(runner,'PORT',Path(temp)), \
                 patch.object(runner.sys,'argv',['run.py']), patch.dict(os.environ,{},clear=True), \
                 patch.object(runtime,'discover',return_value={'vulkan':'found-vk.dll','dx12':'found-dx.dll'}), \
                 patch.object(runner,'refresh_shared_graphics'), \
                 patch.object(runner,'find_executable',return_value=None):
                try:
                    with self.assertRaises(SystemExit): runner.main()
                    self.assertEqual(os.environ['BB_TEST_AMD_VK_RUNTIME'],'found-vk.dll')
                    self.assertEqual(os.environ['BB_TEST_AMD_DX_RUNTIME'],'found-dx.dll')
                finally:
                    os.chdir(cwd)
        finally:
            os.chdir(cwd)


if __name__=='__main__':unittest.main()
