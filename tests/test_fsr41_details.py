# SPDX-License-Identifier: GPL-3.0-or-later
"""Diagnostic causes, privacy, localisation and the actual read-only details widget."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'launcher'), str(ROOT / 'scripts')]
import bbport_fsr41 as family
import bbport_fsr41_details as details
import bbport_fsr41_ui as view
import bbport_fsr411 as legacy
import bbport_fsr412 as driver
import bbport_launcher_win as launcher


def ready():
    status = dict(supported=True, reason='ready', detail='', checks=[])
    return dict(vendor_id=0x1002, gpu=dict(name='AMD Radeon RX 6800 XT',
        vendor_id=0x1002, device_id=0x73bf, driver_name='AMD', driver_info='26.10.1', vulkan='1.3.0'),
        dx12=copy.deepcopy(status), vk=copy.deepcopy(status), dx12_interop=copy.deepcopy(status),
        fsr412_vulkan=copy.deepcopy(status), fsr412_dx12=copy.deepcopy(status))


class DetailsTests(unittest.TestCase):
    def test_all_13_languages_have_complete_diagnostic_labels(self):
        self.assertEqual(set(details.TEXTS), set(family.TEXTS))
        for language, values in details.TEXTS.items():
            self.assertEqual(len(values), len(details.KEYS), language)
            self.assertTrue(all(values), language)
            translate = lambda key: dict(zip(details.KEYS, values)).get(key, key)
            text = details.report(ready(), [dict(supported=True)] * 4, translate)
            self.assertIn(values[1], text)
            for _, _, route, _ in family.ROUTES: self.assertIn(route, text)

    def test_failures_keep_the_native_stage_and_exact_technical_cause(self):
        for stage, cause in (('runtime_file', 'amdvkffx64.dll'),
                ('runtime_load', 'LoadLibraryEx: Windows error 126'),
                ('provider_query', 'UpdateFfxApiProvider: HRESULT=0x80004005'),
                ('provider_version', '4.1.1 / expected 4.1.2'),
                ('provider_support', 'isSupported=false')):
            with self.subTest(stage=stage):
                caps = ready()
                failed = dict(supported=False, reason='driver', detail=cause,
                    checks=[dict(id=stage, state='fail', detail=cause)])
                caps['fsr412_vulkan'] = failed
                states = [failed, *[dict(supported=True)] * 3]
                text = details.report(caps, states, str)
                self.assertIn(details.LABELS[stage] + ': Failed', text)
                self.assertIn(cause, text)
                if stage != 'provider_support': self.assertIn('Provider GPU check: Not checked', text)

    def test_report_allowlist_and_path_redaction_cover_screen_and_clipboard(self):
        caps = ready()
        caps['settings'] = dict(user_name='DO-NOT-EXPORT', token='SECRET')
        caps['fsr412_dx12']['checks'] = [dict(id='runtime_load', state='fail',
            detail="[WinError 126] 'C:\\Users\\Some Person\\AppData\\driver.dll'"),
            dict(id='provider', state='info', detail='/home/private-name/driver.so')]
        text = details.report(caps, [dict(supported=True)] * 4, str)
        for private in ('Some Person', 'AppData', 'private-name', 'DO-NOT-EXPORT', 'SECRET'):
            self.assertNotIn(private, text)
        self.assertIn('<path>', text)
        self.assertIn('WinError 126', text)
        self.assertIn('AMD Radeon RX 6800 XT', text)
        self.assertEqual(text, '\n'.join(value for value, _ in details.lines(caps, [dict(supported=True)] * 4, str)) + '\n')

    def test_file_problem_is_visible_even_when_gpu_check_also_failed(self):
        caps = ready()
        failed = dict(supported=False, reason='hardware', detail='shaderInt8',
            files_problem=dict(reason='download', detail='amd_fidelityfx_upscaler_vk.dll'))
        text = details.report(caps, [dict(supported=True)] * 3 + [failed], str)
        self.assertIn('Package files: Failed', text)
        self.assertIn('amd_fidelityfx_upscaler_vk.dll', text)
        self.assertIn('shaderInt8', text)

    def test_malformed_traces_are_rejected(self):
        for checks in (None, 'text', [dict(id='provider', state='made-up', detail='')],
                       [dict(id='provider', state='pass', detail=42)]):
            self.assertFalse(legacy.valid_status(dict(supported=True, reason='ready', detail='', checks=checks)))

    def test_driver_probe_preserves_trace_and_reports_timeout_or_process_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            port = Path(folder)
            paths = dict(vulkan=str(port / 'amdvkffx64.dll'))
            good = dict(supported=False, reason='hardware', detail='isSupported=false',
                checks=[dict(id='provider_support', state='fail', detail='isSupported=false')], gpu=ready()['gpu'])
            response = subprocess.CompletedProcess([], 0, json.dumps(good), '')
            with patch.object(driver.subprocess, 'run', return_value=response):
                result = driver.query_vulkan(port, paths, refresh=True)
            self.assertEqual(result['checks'][0]['id'], 'runtime_file')
            self.assertEqual(result['checks'][-1]['detail'], 'isSupported=false')
            self.assertEqual(result['gpu']['name'], 'AMD Radeon RX 6800 XT')
            for failure in (subprocess.TimeoutExpired('probe', 15), OSError('probe missing')):
                with patch.object(driver.subprocess, 'run', side_effect=failure):
                    result = driver.query_vulkan(port, paths, refresh=True)
                self.assertFalse(result['supported'])
                self.assertEqual(result['checks'][-1]['state'], 'fail')
            with patch.object(driver.subprocess, 'run', return_value=subprocess.CompletedProcess([], 17, '', '')):
                result = driver.query_vulkan(port, paths, refresh=True)
            self.assertIn('exit=17', result['detail'])


@unittest.skipUnless(sys.platform == 'win32', 'Windows launcher widgets')
class DetailsWidgetsTests(unittest.TestCase):
    def test_settings_details_and_recheck_are_read_only_and_refresh_together(self):
        import tkinter as tk
        from tkinter import ttk
        root = tk.Tk(); root.withdraw()
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.root, app.tk, app.ttk, app.dpi = root, tk, ttk, 1
        app.app, app.ini, app.vars = dict(launcher.APP_DEFAULTS), dict(launcher.INI_DEFAULTS), {}
        app.ini.update(upscaler='fsr412', fsr411_backend='vk', frame_gen='fsr402')
        app.fsr411_refreshers = []; app.index_entry = Mock(); app.process = None
        app.fsr411_downloading = False; app.recheck_fsr411 = Mock(); app.download_fsr411 = Mock()
        app.fsr411_support = ready()
        app.fsr411_status = lambda method: app.fsr411_support[method] if app.fsr411_support else dict(supported=False, reason='checking')
        previous = dict(app.ini)
        try:
            settings = view.open_settings(app, str)
            self.assertEqual(settings.title(), '')
            self.assertEqual(settings.details_button.cget('text'), 'Details')
            settings.details_button.invoke(); root.update()
            dialog = app.fsr41_details_window
            self.assertEqual(dialog.title(), '')
            heading = dialog.winfo_children()[0].winfo_children()[0]
            self.assertEqual(heading.cget('text'), 'FSR 4.1 diagnostics')
            self.assertIn('RX 6800 XT', dialog.report_text)
            self.assertEqual(dialog.text.cget('state'), 'disabled')
            self.assertIs(view.open_details(app, str), dialog)
            # Capture the copy target instead of touching the user's clipboard.
            clear, append = Mock(), Mock()
            with patch.object(root, 'clipboard_clear', clear), patch.object(root, 'clipboard_append', append):
                dialog.copy_button.invoke()
            append.assert_called_once_with(dialog.report_text)
            dialog.recheck_button.invoke(); app.recheck_fsr411.assert_called_once()
            app.fsr411_support = None
            for refresh in list(app.fsr411_refreshers): refresh()
            self.assertTrue(dialog.recheck_button.instate(['disabled']))
            self.assertTrue(dialog.copy_button.instate(['disabled']))
            app.fsr411_support = ready()
            for refresh in list(app.fsr411_refreshers): refresh()
            self.assertFalse(dialog.recheck_button.instate(['disabled']))
            self.assertEqual(app.ini, previous)
            self.assertEqual(app.var('frame_gen', 'ini').get(), 'fsr402')
            dialog.destroy(); settings.destroy()
            self.assertEqual(app.fsr411_refreshers, [])
        finally:
            root.update_idletasks(); root.destroy()


if __name__ == '__main__': unittest.main()
