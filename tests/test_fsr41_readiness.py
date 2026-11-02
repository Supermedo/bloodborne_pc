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
            for key, translated in zip(cache.KEYS, cache.TEXTS[language]):
                self.assertEqual(texts[family.KEYS.index(key)], translated)
            self.assertTrue(all(texts[family.KEYS.index(key)] for key in family.FALLBACK_KEYS))
            self.assertEqual(len(texts),len(family.KEYS))

    def test_automatic_recommendations_use_the_same_verified_route_order(self):
        """An FSR 4 recommendation becomes the first supported FSR 4.1 method; any other stays (R84: fsr4vk also
        runs on a GeForce RTX, whose recommendation is DLSS)."""
        import bbport_setup as setup
        ui=types.SimpleNamespace(rating=object(), app={}, display_size=lambda:(1920,1080),
            current_settings=lambda:{}, var=lambda *_:types.SimpleNamespace(get=lambda:'balanced'),
            fsr411_support=None, set_by_hand=lambda *_a, **_k: False)
        for upscaler in ('fsr4', 'fsr3', 'dlss', 'off'):
            for mask in range(16):
                states=[dict(supported=bool(mask&(1<<i))) for i in range(4)]
                with patch.object(setup.hardware,'outputs',return_value={'1080p':'1920x1080'}), \
                     patch.object(setup.hardware,'recommended_class',return_value='1080p'), \
                     patch.object(setup.hardware,'profile',return_value=[('ini','upscaler',upscaler,'quality')]), \
                     patch.object(family,'method_statuses',return_value=states):
                    _res,_output,entries=setup.SetupMixin.recommended(ui)
                values={key:value for _store,key,value,_why in entries}
                selected=next((i for i in range(4) if states[i]['supported']),None)
                if selected is None or upscaler != 'fsr4':
                    self.assertEqual(values['upscaler'],upscaler)
                    self.assertNotIn('fsr411_backend',values)
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

    def test_a_new_ini_leaves_sharpening_to_the_profile(self):
        """R81: the FSR 4.1 method a new bbport.ini starts with is only a first value (fsr4vk is supported on a
        GeForce RTX too, whose profile brings DLSS). Sharpening keeps its default here: the profile applied for a
        new ini turns it off with FSR 4 only (test_launcher_auto_profile)."""
        with tempfile.TemporaryDirectory() as temp, \
             patch.object(launcher, 'ini_path', return_value=Path(temp)/'bbport.ini'), \
             patch.object(family, 'query', return_value={}), \
             patch.object(launcher.bbport_fsr411, 'default_backend', return_value='vk'):
            path = Path(temp)/'bbport.ini'
            for states, upscaler in (([dict(supported=True)]+[dict(supported=False)]*3, 'fsr412'),
                                     ([dict(supported=False)]*4, 'fsr3')):
                path.unlink(missing_ok=True)
                with patch.object(family, 'method_statuses', return_value=states):
                    values, _ = launcher.load_ini()
                self.assertEqual((values['upscaler'], values['sharpen']), (upscaler, '1'))

    def test_direct_run_removes_legacy_overrides_without_driver_discovery(self):
        spec=importlib.util.spec_from_file_location('run_readiness',ROOT/'run.py')
        runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
        cwd=Path.cwd()
        try:
            with tempfile.TemporaryDirectory() as temp, patch.object(runner,'PORT',Path(temp)), \
                 patch.object(runner.sys,'argv',['run.py']), \
                 patch.dict(os.environ,{'BB_TEST_AMD_VK_RUNTIME':'old.dll','BB_TEST_AMD_DX_RUNTIME':'old.dll','unrelated':'keep'},clear=True), \
                 patch.object(runtime,'discover',side_effect=AssertionError('runner must not discover internal drivers')), \
                 patch.object(runner,'refresh_shared_graphics'), \
                 patch.object(runner,'hold_game_mutex',return_value=True), \
                 patch.object(runner,'find_executable',return_value=None):
                try:
                    with self.assertRaises(SystemExit): runner.main()
                    self.assertNotIn('BB_TEST_AMD_VK_RUNTIME',os.environ)
                    self.assertNotIn('BB_TEST_AMD_DX_RUNTIME',os.environ)
                    self.assertEqual(os.environ['unrelated'],'keep')
                finally:
                    os.chdir(cwd)
        finally:
            os.chdir(cwd)


if __name__=='__main__':unittest.main()
