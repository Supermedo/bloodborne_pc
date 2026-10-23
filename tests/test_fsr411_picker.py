# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual disabled menu entries, package repair and launch boundary without popups."""
import copy
import hashlib
import os
import queue
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'launcher'))
import bbport_launcher_win as launcher
import bbport_fsr411 as support
import bbport_fsr41_ui as picker
import bbport_fsr41 as family

READY = dict(vendor_id=0x1002, **{m: dict(supported=True, reason='ready', detail='') for m in ('vk', 'dx12')})


class PickerFixture:
    def setUp(self):
        import tkinter as tk
        from tkinter import ttk
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        self.payload = b'private verified Vulkan fixture'
        real_fetch = launcher.fetch_fsr4vk
        self.fetch = types.SimpleNamespace(FILES=[('', 'test.dll', len(self.payload),
            hashlib.sha256(self.payload).hexdigest())], verified=real_fetch.verified,
            files_ok=real_fetch.files_ok, FetchError=real_fetch.FetchError, fetch=Mock())
        self.patches = [patch.object(launcher, 'PORT_DIR', self.folder),
            patch.object(launcher, 'DATA_DIR', self.folder), patch.object(launcher, 'fetch_fsr4vk', self.fetch),
            patch.object(real_fetch, 'FILES', self.fetch.FILES),
            patch.dict(os.environ, {'BB_FSR4VK_DIR': str(self.folder/'fsr4vk'), 'BB_FSR4VK':'1'})]
        for p in self.patches:
            p.start(); self.addCleanup(p.stop)
        (self.folder/'bin').mkdir()
        (self.folder/'bin'/support.DLL['dx12']).write_bytes(b'private DX12 fixture')
        ui = self.ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.root, ui.tk, ui.ttk, ui.dpi = self.root, tk, ttk, 1
        ui.app, ui.ini, ui.vars = dict(launcher.APP_DEFAULTS), dict(launcher.INI_DEFAULTS), {}
        ui.ini.update(upscaler='fsr411', fsr411_backend='vk')
        ui.fsr411_support = copy.deepcopy(READY)
        ui.fsr411_refreshers, ui.fsr411_upscaler_refreshers = [], []
        ui.process, ui.fsr411_downloading = None, False
        ui.index_entry = Mock()
        ui.messagebox = types.SimpleNamespace(askyesno=Mock(return_value=False), showerror=Mock())
        self.parent = ttk.Frame(self.root)
        self.pick = ui.fsr411_choice(self.parent)

    def repair(self):
        path = self.folder/'fsr4vk/test.dll'
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(self.payload)
        for refresh in self.ui.fsr411_refreshers: refresh()
        return path


class PickerTests(PickerFixture, unittest.TestCase):
    def test_missing_package_is_disabled_and_settings_remain_accessible(self):
        window=self.ui.open_fsr41_settings()
        self.assertTrue(window.cards[3][1].instate(['disabled']))
        self.assertEqual(window.cards[3][2].cget('text'),launcher._('Files missing'))
        self.assertFalse(self.pick.button.instate(['disabled']))
        window.cards[2][1].invoke()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(),'dx12')
        window.cards[3][1].invoke()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(),'dx12')
        self.ui.messagebox.askyesno.assert_not_called()

    def test_repaired_package_enables_and_corruption_disables_route(self):
        window=self.ui.open_fsr41_settings()
        path=self.repair()
        self.assertFalse(window.cards[3][1].instate(['disabled']))
        window.cards[2][1].invoke();window.cards[3][1].invoke()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(),'vk')
        path.write_bytes(b'changed package')
        for refresh in list(self.ui.fsr411_refreshers):refresh()
        self.assertTrue(window.cards[3][1].instate(['disabled']))
        self.assertEqual(self.ui.vars['fsr411_backend'].get(),'vk')

    def test_other_upscalers_disable_settings_and_preserve_saved_method(self):
        self.repair()
        for upscaler in ('fsr3','taa','off'):
            self.ui.vars['upscaler'].set(upscaler)
            self.assertTrue(self.pick.button.instate(['disabled']))
            self.assertEqual(self.ui.vars['fsr411_backend'].get(),'vk')
        window=self.ui.open_fsr41_settings();window.cards[2][1].invoke()
        self.assertEqual(self.ui.vars['upscaler'].get(),'fsr411')
        self.assertEqual(self.ui.vars['fsr411_backend'].get(),'dx12')

    def test_install_does_not_override_hardware_or_failed_probe(self):
        self.repair();window=self.ui.open_fsr41_settings()
        for reason in ('hardware','check','checking'):
            self.ui.fsr411_support['vk']=dict(supported=False,reason=reason,detail='feature')
            for refresh in list(self.ui.fsr411_refreshers):refresh()
            self.assertTrue(window.cards[3][1].instate(['disabled']))
            self.assertEqual(self.ui.vars['fsr411_backend'].get(),'vk')

    def test_failed_check_keeps_saved_method_and_explains_status(self):
        self.ui.fsr411_support['vk']=dict(supported=False,reason='check',detail='')
        window=self.ui.open_fsr41_settings()
        self.assertEqual(window.cards[3][2].cget('text'),launcher._('Files missing'))
        self.repair()
        self.assertEqual(window.cards[3][2].cget('text'),launcher._('Support check failed'))
        self.assertEqual(self.ui.vars['fsr411_backend'].get(),'vk')

    def test_all_languages_and_two_controls_stay_synchronized(self):
        self.repair();previous=launcher.LANG
        try:
            for lang in family.TEXTS:
                launcher.LANG=lang
                second=self.ui.fsr411_choice(self.parent)
                window=self.ui.open_fsr41_settings()
                self.assertEqual(window.title(), '')
                heading = window.winfo_children()[0].winfo_children()[0]
                self.assertEqual(heading.cget('text'), launcher._('FSR 4.1 Settings'))
                window.cards[2][1].invoke()
                self.assertIn('AMD DX12',second.summary.cget('text'))
                self.assertIn('AMD DX12',self.pick.summary.cget('text'))
                self.assertEqual(window.cards[2][2].cget('text'),launcher._('Supported'))
                window.destroy();second.destroy()
        finally:launcher.LANG=previous

    def test_settings_caption_width_and_destroy_cleanup_in_all_languages(self):
        import tkinter.font as font
        previous=launcher.LANG
        try:
            for lang in family.TEXTS:
                launcher.LANG=lang
                variables=[self.ui.vars[key] for key in ('upscaler','fsr411_backend')]
                traces=[len(var.trace_info()) for var in variables]
                quick=self.ui.fsr411_choice(self.parent,width=26)
                quick.pack();self.parent.pack();self.root.update_idletasks()
                actual=font.Font(root=self.root,font=self.ui.ttk.Style().lookup('TButton','font') or 'TkDefaultFont')
                self.assertGreaterEqual(quick.button.winfo_reqwidth(),actual.measure(quick.button.cget('text')),lang)
                quick.destroy()
                self.assertEqual([len(var.trace_info()) for var in variables],traces)
        finally:launcher.LANG=previous

    def test_play_enabled_only_with_valid_selected_method(self):
        self.ui.process,self.ui.fsr411_downloading,self.ui.gpu_text=None,False,'GPU'
        self.ui.check_line,self.ui.show_hunters,self.ui.test_running=Mock(),Mock(),Mock(return_value=False)
        self.ui.play_button=self.ui.ttk.Button(self.parent);self.ui.status=self.ui.tk.Label(self.parent)
        with patch.object(launcher,'game_info',return_value=('fixture',launcher.PATCH_VERSION)),patch.object(launcher,'fsr4_missing',return_value=[]):
            self.ui.refresh_status();self.assertTrue(self.ui.play_button.instate(['disabled']))
            self.ui.open_fsr41_settings().cards[2][1].invoke()
            self.ui.refresh_status();self.assertFalse(self.ui.play_button.instate(['disabled']))


class DownloadTests(PickerFixture, unittest.TestCase):
    def make_download(self, outcome='success'):
        ui = self.ui
        ui.ui_calls = queue.Queue()
        ui.play_button = ui.ttk.Button(self.parent)
        ui.fsr411_label, ui.fsr411_native_label = ui.ttk.Label(self.parent), ui.ttk.Label(self.parent)
        ui.fsr411_button = ui.ttk.Button(self.parent)
        ui.fsr411_progress = ui.ttk.Progressbar(self.parent)
        ui.refresh_status = Mock()
        ui.messagebox.askyesno.return_value = True
        self.workers = []
        def thread(target, **kwargs):
            return types.SimpleNamespace(start=lambda: self.workers.append(target))
        p = patch.object(launcher.threading, 'Thread', side_effect=thread)
        p.start(); self.addCleanup(p.stop)
        def fetch(folder, progress):
            if outcome == 'error':
                raise self.fetch.FetchError('private download error')
            path = Path(folder)/'test.dll'
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.payload if outcome == 'success' else b'corrupt result')
            progress(len(self.payload), len(self.payload))
        self.fetch.fetch.side_effect = fetch
        ui.var('fsr411_backend','ini').set('dx12')
        ui.download_fsr411()

    def finish_download(self):
        self.workers.pop()()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')
        while not self.ui.ui_calls.empty():
            self.ui.ui_calls.get_nowait()()

    def test_repair_uses_shared_download_and_enables_route_after_verification(self):
        self.make_download()
        self.assertTrue(self.ui.fsr411_downloading)
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')
        self.assertFalse(self.pick.button.instate(['disabled']))
        self.fetch.fetch.assert_not_called()
        self.finish_download()
        self.fetch.fetch.assert_called_once()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')
        self.assertTrue(self.ui.fsr411_status('vk')['supported'])
        self.assertFalse(self.ui.fsr411_downloading)
        self.ui.messagebox.showerror.assert_not_called()

    def test_download_failure_retains_previous_method(self):
        self.make_download('error')
        self.finish_download()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')
        self.assertFalse(self.ui.fsr411_downloading)
        self.ui.messagebox.showerror.assert_called_once()

    def test_unverified_download_retains_previous_method(self):
        self.make_download('invalid')
        self.finish_download()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')
        self.ui.messagebox.showerror.assert_called_once()

    def test_capability_failure_during_download_does_not_activate_vulkan(self):
        self.make_download()
        self.ui.fsr411_support['vk'] = dict(supported=False, reason='hardware', detail='feature')
        self.finish_download()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')

    def test_upscaler_change_during_download_does_not_override_preference(self):
        self.make_download()
        self.ui.vars['upscaler'].set('fsr3')
        self.finish_download()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')

    def test_graphics_button_download_keeps_selected_method(self):
        self.make_download()
        self.finish_download()
        self.ui.var('fsr411_backend','ini').set('dx12')
        self.ui.download_fsr411()
        self.finish_download()
        self.assertEqual(self.ui.vars['fsr411_backend'].get(), 'dx12')


class ShortcutTests(unittest.TestCase):
    def test_invalid_saved_method_returns_to_launcher_without_popup_or_game(self):
        status = dict(supported=False, reason='download', detail='test.dll')
        with patch.object(launcher, 'attach_stdio'), patch.object(launcher, 'load_ini',
                return_value=({'upscaler':'fsr411', 'fsr411_backend':'vk'}, [])), \
             patch.object(support, 'query', return_value=READY), \
             patch.object(support, 'method_status', return_value=status), \
             patch.object(launcher.subprocess, 'Popen') as popen, \
             patch.object(launcher.ctypes.windll.user32, 'MessageBoxW') as popup, patch('builtins.print'):
            self.assertIsNone(launcher.play_without_window(launcher.APP_DEFAULTS))
        popen.assert_not_called()
        popup.assert_not_called()

    def test_shortcut_opens_graphics_when_saved_method_is_unavailable(self):
        import tkinter as tk
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp)/'eboot.bin').touch()
            ui, root = Mock(), Mock()
            with patch.object(launcher.sys, 'argv', ['Bloodborne.exe', '--play']), \
                 patch.object(launcher, 'FROZEN', False), \
                 patch.object(launcher, 'CONFIG_FILE', Path(temp)/'eboot.bin'), \
                 patch.object(launcher, 'load_json', return_value={'game_dir':temp}), \
                 patch.object(launcher, 'play_without_window', return_value=None), \
                 patch.object(tk, 'Tk', return_value=root), \
                 patch.object(launcher, 'Launcher', return_value=ui):
                launcher.main()
            ui.show.assert_called_once_with('graphics')
            root.mainloop.assert_called_once()


if __name__ == '__main__':
    unittest.main()
