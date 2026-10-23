# SPDX-License-Identifier: GPL-2.0-or-later
"""ML-FG selection follows the selected adapter's verified capabilities."""
import importlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
import queue
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'launcher'))
sys.path.insert(0,str(ROOT/'scripts'))
import bbport_fg401 as fg
import bbport_launcher_win as launcher

class CapabilityTests(unittest.TestCase):
    def result(self,stdout,code=0):
        with patch.object(fg.Path, 'is_file', return_value=True), \
             patch.object(fg.subprocess,'run',return_value=types.SimpleNamespace(returncode=code,stdout=stdout)):
            return fg.query(ROOT)

    def test_exact_verified_provider(self):
        self.assertTrue(self.result('{"supported":true,"provider":"4.0.1"}')['supported'])

    def test_driver_provider_requires_matching_version(self):
        with patch.object(fg.Path, 'is_file', return_value=True), \
             patch.object(fg.subprocess, 'run', return_value=types.SimpleNamespace(
                 returncode=0, stdout='{"supported":true,"provider":"4.0.2"}')) as run:
            self.assertTrue(fg.query(ROOT, version='4.0.2')['supported'])
            self.assertEqual(run.call_args.args[0][-2:], ['--provider', '4.0.2'])
            self.assertFalse(fg.query(ROOT)['supported'])

    def test_unavailable_or_other_provider(self):
        for stdout in ['{}','[]','null','broken','{"supported":false}',
                       '{"supported":true,"provider":"3.1.6"}',
                       '{"supported":1,"provider":"4.0.1"}']:
            with self.subTest(stdout=stdout):
                self.assertFalse(self.result(stdout)['supported'])
        self.assertFalse(self.result('{"supported":true,"provider":"4.0.1"}',1)['supported'])

    def test_missing_and_timeout(self):
        for error in [FileNotFoundError(),subprocess.TimeoutExpired('probe',15)]:
            with self.subTest(error=error), patch.object(fg.Path, 'is_file', return_value=True), \
                 patch.object(fg.subprocess,'run',side_effect=error):
                self.assertFalse(fg.query(ROOT)['supported'])

    def test_missing_helper_is_a_package_error(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(fg.query(folder), dict(supported=False, reason='files',
                                                  detail='bb-fg401-capabilities.exe'))

    def test_failure_reasons_and_file_detail(self):
        for reason in fg.REASONS - {'ready'}:
            result = self.result('{"supported":false,"reason":"'+reason+'",'
                                 '"detail":"test.dll","provider":"4.0.1"}')
            self.assertEqual(result, dict(supported=False, reason=reason, detail='test.dll'))
        for payload in ['{"supported":false,"reason":"unknown","provider":"4.0.1"}',
                        '{"supported":false,"reason":"files","detail":{},"provider":"4.0.1"}']:
            self.assertEqual(self.result(payload)['reason'], 'check')

    def test_concise_messages_and_hdr_condition(self):
        translate = lambda key: key
        self.assertEqual(fg.message(dict(supported=True), translate), '')
        self.assertEqual(fg.message(dict(supported=True), translate, hdr=True), '')
        self.assertEqual(fg.message(dict(supported=True), translate, selected=True), '')
        self.assertEqual(fg.message(dict(supported=True), translate, hdr=True, selected=True),
                         'ML-FG pauses during HDR output.')
        for reason, word in [('windows11', 'Windows 11'), ('rx9000', 'RX 9000'),
                             ('files', 'test.dll'), ('runtime', 'test.dll'),
                             ('check', 'Recheck support')]:
            self.assertIn(word, fg.message(dict(supported=False, reason=reason, detail='test.dll'), translate))

    def test_translations_and_mode_label(self):
        self.assertEqual(len(fg.TEXTS),13)
        previous=launcher.LANG
        try:
            for language in fg.TEXTS:
                launcher.LANG=language
                for key in fg.KEYS:
                    self.assertEqual(launcher._(key),fg.TRANSLATIONS[language][key])
                    self.assertEqual(key.count('{}'), fg.TRANSLATIONS[language][key].count('{}'))
                    self.assertTrue(launcher._(key).format('test.dll'))
                    if '4.0.1' in key:
                        new_key = key.replace('4.0.1', '4.0.2')
                        self.assertEqual(launcher._(new_key),
                                         fg.TRANSLATIONS[language][key].replace('4.0.1', '4.0.2'))
                label=fg.TEXTS[language][0]
                self.assertIn('RX 9000',label)
                self.assertIn('2×',label)
                self.assertIn('SDR',label)
            self.assertFalse(any('ready (' in key for key in fg.KEYS))
        finally:
            launcher.LANG=previous

class SelectionTests(unittest.TestCase):
    def setUp(self):
        try:
            import tkinter as tk
            from tkinter import ttk
            self.root=tk.Tk()
        except Exception as error:
            self.skipTest(f'Tk session unavailable: {error}')
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        obj=launcher.Launcher.__new__(launcher.Launcher)
        obj.tk=tk;obj.ttk=ttk;obj.root=self.root;obj.vars={}
        obj.ini=launcher.INI_DEFAULTS.copy();obj.app=launcher.APP_DEFAULTS.copy()
        obj.fg401_support=None;obj.fg401_refreshers=[]
        self.parent=ttk.Frame(self.root)
        obj.fg401_status=ttk.Label(self.parent)
        obj.fg401_status.grid(row=1,column=0)
        self.obj=obj
        self.box=obj.fg401_choice(self.parent)
        self.var=obj.var('frame_gen','ini')

    def test_available_only_after_probe(self):
        self.assertFalse(any('4.0.1' in text for text in self.box['values']))
        self.obj.fg401_support={'supported':True}
        self.obj.refresh_fg401()
        self.assertTrue(any('4.0.1' in text for text in self.box['values']))
        self.assertEqual(self.obj.fg401_status.cget('text'),'')
        self.assertEqual(self.obj.fg401_status.winfo_manager(),'')

    def test_stored_selection_survives_failed_check(self):
        self.var.set('fsr401')
        self.assertEqual(self.var.get(),'fsr401')
        self.obj.fg401_support={'supported':False, 'reason':'files', 'detail':'test.dll'}
        self.obj.refresh_fg401()
        self.assertEqual(self.var.get(),'fsr401')
        self.assertFalse(any('4.0.1' in text for text in self.box['values']))
        self.assertIn('4.0.1', self.box.get())
        self.assertIn('test.dll', self.obj.fg401_status.cget('text'))
        self.assertEqual(self.obj.fg401_status.winfo_manager(),'grid')

    def test_failed_probe_does_not_replace_saved_ml_preference(self):
        self.var.set('fsr401')
        self.obj.fg401_support={'supported':False, 'reason':'check'}
        self.obj.refresh_fg401()
        self.obj.ini_lines=[]
        self.obj.mod_order=[];self.obj.patch_vars={}
        self.obj.remap_edits={};self.obj.mouse_edited=False
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            with patch.dict(os.environ, BB_CONFIG=str(folder/'bbport.ini')), \
                 patch.multiple(launcher, CONFIG_DIR=folder, CONFIG_FILE=folder/'settings.json'):
                self.obj.collect()
                self.assertEqual(launcher.load_ini()[0]['frame_gen'], 'fsr401')
                self.assertEqual(self.var.get(), 'fsr401')

    def test_recheck_can_fail_and_recover_without_changing_selection(self):
        self.var.set('fsr401')
        self.obj.ui_calls=queue.Queue()
        def thread(target, **kwargs):
            return types.SimpleNamespace(start=target)
        for status in [dict(supported=False, reason='check'), dict(supported=True)]:
            with patch.object(launcher.threading, 'Thread', side_effect=thread), \
                 patch.object(fg, 'query_versions', return_value=status):
                self.obj.recheck_fg401()
            self.assertEqual(self.var.get(), 'fsr401')
            self.obj.ui_calls.get_nowait()()
            self.assertEqual(self.var.get(), 'fsr401')
            self.assertEqual(self.obj.fg401_support, status)

    def test_hdr_hint_does_not_change_ml_selection(self):
        self.obj.fg401_support={'supported':True}
        self.var.set('fsr401')
        self.obj.var('hdr', 'app').set(True)
        self.obj.refresh_fg401()
        self.assertIn('HDR', self.obj.fg401_status.cget('text'))
        self.assertEqual(self.var.get(), 'fsr401')
        self.obj.var('hdr', 'app').set(False)
        self.obj.refresh_fg401_status()
        self.assertEqual(self.obj.fg401_status.cget('text'), '')

    def test_existing_selection_preserved(self):
        for supported in [True,False]:
            self.obj.fg401_support={'supported':supported}
            for value in ['off','fsr3','dlss']:
                self.var.set(value)
                self.obj.refresh_fg401()
                self.assertEqual(self.var.get(),value)

    def test_providers_are_independently_available(self):
        self.obj.fg401_support = {'fsr401': dict(supported=True),
                                 'fsr402': dict(supported=False, reason='provider')}
        self.obj.refresh_fg401()
        self.assertTrue(any('4.0.1' in text for text in self.box['values']))
        self.assertFalse(any('4.0.2' in text for text in self.box['values']))
        self.obj.fg401_support['fsr402'] = dict(supported=True)
        self.obj.refresh_fg401()
        self.assertTrue(any('4.0.2' in text for text in self.box['values']))
        self.var.set('fsr402')
        self.obj.var('hdr', 'app').set(True)
        self.obj.refresh_fg401()
        self.assertIn('HDR', self.obj.fg401_status.cget('text'))
        self.obj.fg401_support['fsr402'] = dict(supported=False, reason='provider')
        self.obj.refresh_fg401()
        self.assertEqual(self.var.get(), 'fsr402')
        self.assertIn('4.0.2', self.obj.fg401_status.cget('text'))
        self.assertIn('4.0.2', self.box.get())

if __name__=='__main__':
    unittest.main()
