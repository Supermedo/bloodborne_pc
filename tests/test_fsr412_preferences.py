# SPDX-License-Identifier: GPL-3.0-or-later
"""Fresh provider defaults and preservation of explicit compatibility choices."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'launcher'), str(ROOT/'scripts')]
import bbport_launcher_win as launcher
import bbport_fsr412 as provider


class PreferenceTests(unittest.TestCase):
    def test_fresh_default_requires_successful_provider_check(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'bbport.ini'
            with patch.object(launcher, 'ini_path', return_value=path), \
                 patch.object(launcher.bbport_fsr411, 'default_backend', return_value='dx12'):
                for mask in range(16):
                    states = [dict(supported=bool(mask & (1 << i))) for i in range(4)]
                    with patch.object(launcher.bbport_fsr41, 'query', return_value={}), \
                         patch.object(launcher.bbport_fsr41, 'method_statuses', return_value=states):
                        values, _ = launcher.load_ini()
                    expected = next((row[:2] for i, row in enumerate(launcher.bbport_fsr41.ROUTES)
                                     if states[i]['supported']), ('fsr3','dx12'))
                    self.assertEqual((values['upscaler'], values['fsr411_backend']), expected)
                    self.assertEqual(values['fsr_compatibility_options'], '0')
                self.assertFalse(path.exists())  # Reading preferences leaves the file untouched.

    def test_explicit_preferences_survive_hardware_and_visibility_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'bbport.ini'
            with patch.object(launcher, 'ini_path', return_value=path), \
                 patch.object(provider, 'query_vulkan') as probe:
                for upscaler in ('off', 'fsr3', 'fsr4', 'fsr411', 'fsr412'):
                    for backend in ('vk', 'dx12'):
                        for compatibility in ('0', '1'):
                            text = f'upscaler={upscaler}\nfsr411_backend={backend}\nfsr_compatibility_options={compatibility}\n'
                            path.write_text(text, encoding='utf-8')
                            values, _ = launcher.load_ini({'fsr411_backend': 'dx12'})
                            self.assertEqual((values['upscaler'], values['fsr411_backend'],
                                values['fsr_compatibility_options']), (upscaler, backend, compatibility))
                            self.assertEqual(path.read_text(encoding='utf-8'), text)
                probe.assert_not_called()

    def test_provider_response_validation_cache_and_selected_gpu(self):
        with tempfile.TemporaryDirectory() as temp:
            port = Path(temp)
            driver = port/'driver.dll'; driver.write_bytes(b'driver')
            paths = {'vulkan': str(driver)}
            provider._cache.clear()
            result = subprocess.CompletedProcess([], 0, 'provider diagnostics\n{"supported":true,"reason":"ready","detail":"4.1.2"}\n', '')
            with patch.object(provider.subprocess, 'run', return_value=result) as run:
                self.assertTrue(provider.query_vulkan(port, paths, {'BB_GPU_ID': '1'})['supported'])
                self.assertTrue(provider.query_vulkan(port, paths, {'BB_GPU_ID': '1'})['supported'])
                self.assertEqual(run.call_count, 1)
                self.assertEqual(run.call_args.kwargs['env']['BB_GPU_ID'], '1')
                provider.query_vulkan(port, paths, {'BB_GPU_ID': '2'})
                self.assertEqual(run.call_count, 2)
                provider.query_vulkan(port, paths, {'BB_GPU_ID': '2'}, refresh=True)
                self.assertEqual(run.call_count, 3)
            for code, text in ((0, '{"supported":"true"}'), (1, result.stdout), (0, 'no JSON')):
                with patch.object(provider.subprocess, 'run', return_value=subprocess.CompletedProcess([], code, text, '')):
                    self.assertFalse(provider.query_vulkan(port, paths, refresh=True)['supported'])
            caps = {'vendor_id': 0x1002, 'fsr412_vulkan': dict(supported=False, reason='hardware', detail='test')}
            self.assertFalse(provider.method_status('vk', caps, paths)['supported'])


if __name__ == '__main__':
    unittest.main()
