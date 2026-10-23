# SPDX-License-Identifier: GPL-2.0-or-later
"""File diagnosis, capability failures and selection persistence at the launch boundary."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import bbport_launcher_win as launcher
import bbport_fsr411 as support

READY = {'vendor_id': 0x1002, **{m: dict(supported=True, reason='ready', detail='') for m in ('vk', 'dx12')}}


class DetectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = b'verified provider'
        self.fetch = types.SimpleNamespace(FILES=[('', support.DLL['vk'], len(self.payload),
            hashlib.sha256(self.payload).hexdigest())], verified=launcher.fetch_fsr4vk.verified)
        support._cache.clear()

    def install(self, method):
        target = self.root / ('fsr4vk' if method == 'vk' else 'bin') / support.DLL[method]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(self.payload)
        return target

    def status(self, method, caps=READY, env=None):
        return support.method_status(self.root, self.root, method, caps, self.fetch, env or {})

    def test_missing_vulkan_package_names_file_instead_of_blaming_gpu(self):
        for caps in (None, READY):
            status = self.status('vk', caps)
            self.assertEqual(status['reason'], 'download')
            self.assertIn(support.DLL['vk'], support.message(status, str))
            self.assertNotIn('GPU', support.message(status, str))

    def test_missing_files_and_missing_hardware_are_both_reported(self):
        caps = {m: dict(supported=False, reason='hardware', detail='GPU feature') for m in ('vk', 'dx12')}
        for method in ('vk', 'dx12'):
            status = self.status(method, caps)
            self.assertEqual(status['reason'], 'hardware')
            text = support.message(status, str)
            self.assertIn('GPU feature', text)
            self.assertIn(support.DLL[method], text)

    def test_package_verification_and_hardware_are_separate(self):
        dll = self.install('vk')
        self.assertTrue(self.status('vk')['supported'])
        caps = copy.deepcopy(READY)
        caps['vk'] = dict(supported=False, reason='hardware', detail='VK_EXT_descriptor_buffer')
        status = self.status('vk', caps)
        self.assertEqual(status['reason'], 'hardware')
        self.assertIn('VK_EXT_descriptor_buffer', support.message(status, str))
        dll.write_bytes(b'corrupt')
        self.assertEqual(self.status('vk', caps)['files_problem']['reason'], 'download')

    def test_vulkan_directory_override_and_disable(self):
        dll = self.install('vk')
        folder = self.root / 'persistent'
        folder.mkdir()
        dll.replace(folder / dll.name)
        self.assertEqual(self.status('vk')['reason'], 'download')
        self.assertTrue(self.status('vk', env={'BB_FSR4VK_DIR': str(folder)})['supported'])
        self.assertEqual(self.status('vk', env={'BB_FSR4VK_DIR': str(folder), 'BB_FSR4VK': '0'})['reason'], 'disabled')

    def test_missing_dx12_runtime_and_failed_provider_are_distinct(self):
        self.assertEqual(self.status('dx12')['reason'], 'files')
        self.install('dx12')
        self.assertEqual(self.status('dx12', None)['reason'], 'checking')
        for reason in ('provider', 'runtime', 'hardware', 'interop', 'adapter', 'check'):
            caps = copy.deepcopy(READY)
            caps['dx12'] = dict(supported=False, reason=reason, detail='specific cause')
            status = self.status('dx12', caps)
            self.assertEqual(status['reason'], reason)
            self.assertFalse(status['supported'])

    def test_file_lookup_matches_probe_and_game_working_directory(self):
        self.assertEqual(support.vulkan_folder(self.root, self.root, {'BB_FSR4VK_DIR': 'assets'}), self.root/'assets')
        self.assertEqual(support.vulkan_folder(self.root, self.root, {'BB_DATA_DIR': 'data'}), self.root/'data/fsr4vk')
        dll = self.install('dx12')
        (dll.parent/'bb-gpu-capabilities.exe').write_bytes(b'probe')
        out = self.root/'out'
        out.mkdir()
        dll.replace(out/dll.name)
        self.assertEqual(self.status('dx12')['reason'], 'files')

    def test_default_uses_selected_adapter_vendor_and_preserves_saved_choice(self):
        ini = self.root / 'bbport.ini'
        for vendor, expected in ((0x1002, 'dx12'), (0x10de, 'vk'), (0x8086, 'vk'), (None, 'vk')):
            caps = {**READY, 'vendor_id': vendor}
            with patch.object(support, 'query', return_value=caps), \
                 patch.object(launcher.bbport_fsr41, 'query', return_value=caps), \
                 patch.object(launcher.bbport_fsr41, 'method_statuses', return_value=[
                     dict(supported=False), dict(supported=False),
                     dict(supported=vendor == 0x1002), dict(supported=True)]), \
                 patch.object(launcher, 'ini_path', return_value=ini):
                self.assertEqual(support.default_backend(self.root), expected)
                ini.write_text('upscaler=fsr411\n# retain\n')
                before = ini.read_bytes()
                self.assertEqual(launcher.load_ini()[0]['fsr411_backend'], expected)
                self.assertEqual(ini.read_bytes(), before)  # Defaults do not write a profile.
                for chosen in ('vk', 'dx12'):
                    ini.write_text('fsr411_backend=' + chosen + '\n')
                    self.assertEqual(launcher.load_ini()[0]['fsr411_backend'], chosen)
                    ini.write_text('sharpness=0.50\n')
                    values, _ = launcher.load_ini({'fsr411_backend': chosen})
                    self.assertEqual(values['fsr411_backend'], chosen)
                    self.assertIn('sharpness=0.50', ini.read_text())

    def test_probe_validates_output_and_retries_failed_checks(self):
        result = types.SimpleNamespace(returncode=0, stdout=json.dumps(READY))
        with patch.object(support.subprocess, 'run', return_value=result) as run:
            self.assertEqual(support.query(self.root, {}), READY)
            self.assertEqual(support.query(self.root, {}), READY)
            self.assertEqual(run.call_count, 1)
            support.query(self.root, {'BB_GPU_ID': '1'})
            self.assertEqual(run.call_args.kwargs['env']['BB_GPU_ID'], '1')
            support.query(self.root, {}, refresh=True)
            self.assertEqual(run.call_count, 3)
        for bad in ('{}', '[]', 'false', '{', json.dumps({**READY, 'vk': {'supported': 'yes'}})):
            support._cache.clear()
            with patch.object(support.subprocess, 'run', return_value=types.SimpleNamespace(returncode=0, stdout=bad)) as run:
                self.assertEqual(support.query(self.root, {})['vk']['reason'], 'check')
                support.query(self.root, {})
                self.assertEqual(run.call_count, 2)
        with patch.object(support.subprocess, 'run', side_effect=subprocess.TimeoutExpired('probe', 15)):
            self.assertFalse(support.query(self.root, {}, refresh=True)['dx12']['supported'])

    def test_launcher_stops_missing_provider_before_starting_game(self):
        ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.process = None
        ui.fsr411_downloading = False
        ui.app = dict(launcher.APP_DEFAULTS)
        ui.ini = {'upscaler': 'fsr411', 'fsr411_backend': 'vk'}
        ui.vars = {}
        ui.var = lambda *args: types.SimpleNamespace(get=lambda: '' if args[0] != 'online_pool' else 'supermedo')
        for method in ('collect', 'show', 'refresh_status', 'refresh_fsr411'):
            setattr(ui, method, unittest.mock.Mock())
        ui.messagebox = types.SimpleNamespace(showwarning=unittest.mock.Mock())
        status = dict(supported=False, reason='download', detail=support.DLL['vk'])
        ui.fsr41_status = lambda method: status
        with patch.object(launcher.bbport_fsr41, 'query', return_value=READY), \
             patch.object(launcher.subprocess, 'Popen') as popen:
            ui.play()
        popen.assert_not_called()
        ui.show.assert_called_once_with('graphics')
        ui.messagebox.showwarning.assert_not_called()
        self.assertEqual(ui.ini['fsr411_backend'], 'vk')

    def test_messages_cover_all_launcher_languages(self):
        self.assertEqual({lang for lang, _ in launcher.UI_LANGUAGES if lang}, set(support.TEXTS))
        for lang in support.TEXTS:
            table = launcher.bbport_lang.table(lang)
            for key in support.KEYS:
                self.assertIn(key, table)
                self.assertEqual(table[key].count('{}'), key.count('{}'))


@unittest.skipUnless(sys.platform == 'win32', 'Windows launcher widgets')
class SelectionWidgetsTest(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        from tkinter import ttk
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        ui = self.ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.root, ui.tk, ui.ttk, ui.dpi = self.root, tk, ttk, 1
        ui.app, ui.ini, ui.vars = dict(launcher.APP_DEFAULTS), dict(launcher.INI_DEFAULTS), {}
        ui.ini.update(upscaler='fsr3', fsr411_backend='vk')
        ui.fsr411_refreshers, ui.fsr411_upscaler_refreshers = [], []
        ui.fsr411_support = {m: dict(supported=False, reason='hardware', detail='required feature')
                             for m in ('vk', 'dx12')}
        ui.fsr411_status = lambda method: ui.fsr411_support[method]
        self.parent = ttk.Frame(self.root)

    def test_unsupported_methods_are_disabled_and_saved_selection_is_retained(self):
        ui = self.ui
        upscale = ui.choice(self.parent, 'upscaler', 'ini', launcher.UPSCALERS)
        labels=[upscale.menu.entrycget(i,'label') for i in range(upscale.menu.index('end')+1)]
        family=labels.index(launcher._('FSR 4.1 (experimental)'))
        self.assertEqual(upscale.menu.entrycget(family,'state'),'disabled')
        ui.vars['upscaler'].set('fsr411')
        self.assertEqual(upscale.button.cget('text'),launcher._('FSR 4.1 (experimental)'))
        ui.index_entry=lambda *a,**k:None
        methods = ui.fsr411_choice(self.parent)
        dialog=ui.open_fsr41_settings()
        self.assertTrue(dialog.cards[2][1].instate(['disabled']))
        self.assertFalse(methods.button.instate(['disabled']))
        self.assertEqual(ui.vars['fsr411_backend'].get(), 'vk')
        ui.fsr411_support = copy.deepcopy(READY)
        for refresh in ui.fsr411_refreshers + ui.fsr411_upscaler_refreshers:
            refresh()
        self.assertFalse(dialog.cards[2][1].instate(['disabled']))
        ui.vars['upscaler'].set('fsr3')
        self.assertEqual(methods.button.cget('text'),launcher._('FSR 4.1 Settings')+' …')
        self.assertEqual(ui.vars['fsr411_backend'].get(), 'vk')

    def test_unverified_probe_hides_new_selection_but_keeps_saved_selection(self):
        ui = self.ui
        ui.fsr411_support = {m: dict(supported=False, reason='check', detail='') for m in ('vk', 'dx12')}
        upscale = ui.choice(self.parent, 'upscaler', 'ini', launcher.UPSCALERS)
        labels=[upscale.menu.entrycget(i,'label') for i in range(upscale.menu.index('end')+1)]
        family=labels.index(launcher._('FSR 4.1 (experimental)'))
        self.assertEqual(upscale.menu.entrycget(family,'state'),'disabled')
        ui.vars['upscaler'].set('fsr411')
        self.assertEqual(upscale.button.cget('text'),launcher._('FSR 4.1 (experimental)'))


if __name__ == '__main__':
    unittest.main()
